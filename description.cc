/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:
#ident "Copyright (c) 2012-2013 Tokutek Inc.  All rights reserved."
#ident "$Id$"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "backup_debug.h"
#include "manager.h"
#include "mutex.h"
#include "description.h"
#include "real_syscalls.h"
#include "source_file.h"


const int DEST_FD_INIT = -1;

///////////////////////////////////////////////////////////////////////////////
//
// description() -
//
// Description: 
//
//     ...
//
description::description()
: m_offset(0),
  m_fd_in_dest_space(DEST_FD_INIT), 
  m_backup_name(NULL),
  m_source_file(NULL),
  m_in_source_dir(false)
{
}

description::~description(void)
{
    if (m_backup_name) {
        free(m_backup_name);
        m_backup_name = NULL;
    }
}

///////////////////////////////////////////////////////////////////////////////
// See description.h for specification.
int description::init(void)
{
    int r = pthread_mutex_init(&m_mutex, NULL);
    if (r != 0) {
        the_manager.fatal_error(r, "Failed to initialize mutex: %s:%d\n", __FILE__, __LINE__);
    }
    return r;
}

///////////////////////////////////////////////////////////////////////////////
//
void description::set_source_file(source_file *file)
{
    m_source_file = file;
}

///////////////////////////////////////////////////////////////////////////////
//
source_file * description::get_source_file(void) const
{
    return m_source_file;
}

///////////////////////////////////////////////////////////////////////////////
//
void description::prepare_for_backup(const char *name)
{
    char *temp = m_backup_name;
    m_backup_name = strdup(name);
    if (temp != NULL) {
        free((void*)temp);
    }

    __sync_bool_compare_and_swap(&m_in_source_dir, false, true);
}

///////////////////////////////////////////////////////////////////////////////
//
void description::disable_from_backup(void)
{
    // Set this atomically.  It's a little bit overkill, but the atomic primitives didn't convince drd to keep quiet.
    // __sync_lock_release(&m_in_source_dir); // this is a way to write a zero atomically.  drd won't keep quiet here either.
    __sync_bool_compare_and_swap(&m_in_source_dir, true, false);
    //bool new_false = false; __atomic_store(&m_in_source_dir, &new_false, __ATOMIC_SEQ_CST);
}

///////////////////////////////////////////////////////////////////////////////
//
//const char * description::set_full_source_name(void)
//{


///////////////////////////////////////////////////////////////////////////////
//
const char * description::get_full_source_name(void)
{
    const char * result = NULL;
    if (m_source_file != NULL) {
        result = m_source_file->name();
    }

    return result;
}

///////////////////////////////////////////////////////////////////////////////
//
int description::lock(void)
{
    return pmutex_lock(&m_mutex);
}

///////////////////////////////////////////////////////////////////////////////
//
int description::unlock(void)
{
    return pmutex_unlock(&m_mutex);
}

///////////////////////////////////////////////////////////////////////////////
//
// open() -
//
// Description: 
//
//     Calls the operating system's open() syscall for the current
// file description.  This also sets the file descriptor in the 
// destination/backup space for the backup copy of the original file.
//
// Notes:
//
//     Open assumes that the backup file exists.  Create assumes the 
// backup file does NOT exist.
//
int description::open(void)
{
    int r = 0;
    int fd = 0;
    fd = call_real_open(m_backup_name, O_WRONLY, 0777);
    if (fd < 0) {
        int error = errno;

        // For now, don't store the fd if they are opening a dir.
        // That is just for fsync'ing a dir, which we do not care about.
        if(error == EISDIR) {
            goto out;
        }

        if(error != ENOENT && error != EISDIR) {
            r = error;
            goto out;
        }

        r = this->create();
    } else {
        this->m_fd_in_dest_space = fd;
    }
    
out:
    return r;
}

///////////////////////////////////////////////////////////////////////////////
//
// create() -
//
// Description: 
//
//     Calls the operating system's open() syscall with the create
// flag for the current file description.  This also sets the file 
// descriptor in the destination/backup space for the backup
// copy of the original file.
//
// Notes:
//
//     Open assumes that the backup file exists.  Create assumes the 
// backup file does NOT exist.
//
int description::create(void)
{
    int r = 0;
    // Create file that was just opened, this assumes the parent directories
    // exist.
    int fd = 0;
    fd = call_real_open(m_backup_name, O_CREAT | O_WRONLY, 0777);
    if (fd < 0) {
        int error = errno;
        if(error != EEXIST) {
            r = -1;
            goto out;
        }
        fd = call_real_open(m_backup_name, O_WRONLY, 0777);
        if (fd < 0) {
            perror("ERROR: <CAPTURE>: Couldn't open backup copy of recently opened file.");
            r = -1;
            goto out;
        }
    }

    this->m_fd_in_dest_space = fd;

out:
    return r;
}

///////////////////////////////////////////////////////////////////////////////
//
// close() -
//
// Description: 
//
//     ...
//
int description::close(void)
{
    int r = 0;
    if(!m_in_source_dir) {
        goto out;
    }
    
    if(m_fd_in_dest_space == DEST_FD_INIT) {
        goto out;
    }

    // TODO: #6544 Check refcount, if it's zero we REALLY have to close
    // the file.  Otherwise, if there are any references left, 
    // we can only decrement the refcount; other file descriptors
    // are still open in the main application.
    {
        int r2 = call_real_close(m_fd_in_dest_space);
        if (r2==-1) {
            r = errno;
            the_manager.backup_error(r, "Trying to close a backup file (fd=%d)", m_fd_in_dest_space);
        }
    }
out:    
    return r;
}

///////////////////////////////////////////////////////////////////////////////
//
void description::increment_offset(ssize_t nbyte) {    
    m_offset += nbyte;
}

off_t description::get_offset(void) {    
    return m_offset;
}

///////////////////////////////////////////////////////////////////////////////
//
// seek() -
//
// Description: 
//
//     ...
//
void description::lseek(off_t new_offset) {
    m_offset = new_offset;
}

///////////////////////////////////////////////////////////////////////////////
// Description: See description.h
// Note: Returns 0 on success, otherwise an error number (in which case the error is reported)
int description::pwrite(const void *buf, size_t nbyte, off_t offset)
{
    if(!m_in_source_dir) {
        return 0;
    }
    
    if(m_fd_in_dest_space == DEST_FD_INIT) {
        return 0;
    }
    
    // Get the data written out, or do 
    while (nbyte>0) {
        ssize_t wr = call_real_pwrite(this->m_fd_in_dest_space, buf, nbyte, offset);
        if (wr==-1) {
            int r = errno;
            the_manager.backup_error(r, "Failed to pwrite backup file at %s:%d", __FILE__, __LINE__);
            return r;
        }
        if (wr==0) {
            // Can this happen?  Don't see how.  If it does happen, treat it as an error.
            int r = -1; // Unknown error
            the_manager.backup_error(-1, "pwrite inexplicably returned zero at %s:%d", __FILE__, __LINE__);
            return r;
        }
        nbyte -= wr;
        offset += wr;
    }
    return 0;
}

///////////////////////////////////////////////////////////////////////////////
//
// truncate() -
//
// Description: 
//
//     ...
//
int description::truncate(off_t length)
{
    int r = 0;
    if(!m_in_source_dir) {
        goto out;
    }

    if (m_fd_in_dest_space == DEST_FD_INIT) {
        goto out;
    }

    r = call_real_ftruncate(this->m_fd_in_dest_space, length);
    if (r != 0) {
        r = errno;
        the_manager.backup_error(r, "Truncating backup file failed at %s:%d", __FILE__, __LINE__);
    }

out:    
    return r;
}