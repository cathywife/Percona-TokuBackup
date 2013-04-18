/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:  
#ident "Copyright (c) 2012-2013 Tokutek Inc.  All rights reserved."
#ident "$Id$"

#include "backup_debug.h"
#include "file_hash_table.h"
#include "glassbox.h"
#include "manager.h"
#include "real_syscalls.h"
#include "source_file.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <valgrind/helgrind.h>

#if DEBUG_HOTBACKUP
#define WARN(string, arg) HotBackup::CaptureWarn(string, arg)
#define TRACE(string, arg) HotBackup::CaptureTrace(string, arg)
#define ERROR(string, arg) HotBackup::CaptureError(string, arg)
#else
#define WARN(string, arg) 
#define TRACE(string, arg) 
#define ERROR(string, arg) 
#endif

#if PAUSE_POINTS_ON
#define PAUSE(number) while(HotBackup::should_pause(number)) { sleep(2); } printf("Resuming from Pause Point.\n");
#else
#define PAUSE(number)
#endif

//////////////////////////////////////////////////////////////////////////////
//
static void print_time(const char *toku_string) {
    time_t t;
    char buf[27];
    time(&t);
    ctime_r(&t, buf);
    fprintf(stderr, "%s %s\n", toku_string, buf);
}

pthread_mutex_t manager::m_mutex         = PTHREAD_MUTEX_INITIALIZER;
pthread_rwlock_t manager::m_session_rwlock = PTHREAD_RWLOCK_INITIALIZER;
pthread_mutex_t manager::m_error_mutex   = PTHREAD_MUTEX_INITIALIZER;

///////////////////////////////////////////////////////////////////////////////
//
// manager() -
//
// Description: 
//
//     Constructor.
//
manager::manager(void)
    : 
#ifdef GLASSBOX
      m_pause_disable(false),
      m_start_copying(true),
      m_keep_capturing(false),
      m_is_capturing(false),
      m_done_copying(false),
#endif
      m_backup_is_running(false),
      m_session(NULL),
      m_throttle(ULONG_MAX),
      m_an_error_happened(false),
      m_errnum(BACKUP_SUCCESS),
      m_errstring(NULL)
{
    VALGRIND_HG_DISABLE_CHECKING(&m_backup_is_running, sizeof(m_backup_is_running));
#ifdef GLASSBOX
    VALGRIND_HG_DISABLE_CHECKING(&m_is_capturing, sizeof(m_is_capturing));
    VALGRIND_HG_DISABLE_CHECKING(&m_done_copying, sizeof(m_done_copying));
    VALGRIND_HG_DISABLE_CHECKING(&m_an_error_happened, sizeof(m_an_error_happened));
    VALGRIND_HG_DISABLE_CHECKING(&m_errnum, sizeof(m_errnum));
    VALGRIND_HG_DISABLE_CHECKING(&m_errstring, sizeof(m_errstring));
#endif
}

manager::~manager(void) {
    if (m_errstring) free(m_errstring);
}

///////////////////////////////////////////////////////////////////////////////
//
// do_backup() -
//
// Description: 
//
//     
//
int manager::do_backup(const char *source, const char *dest, backup_callbacks *calls) {
    int r = 0;
    if (this->is_dead()) {
        calls->report_error(-1, "Backup system is dead");
        r = -1;
        goto error_out;
    }
    m_an_error_happened = false;
    m_backup_is_running = true;
    r = calls->poll(0, "Preparing backup");
    if (r != 0) {
        calls->report_error(r, "User aborted backup");
        goto error_out;
    }

    r = pthread_mutex_trylock(&m_mutex);
    if (r != 0) {
        if (r==EBUSY) {
            calls->report_error(r, "Another backup is in progress.");
            goto error_out;
        }
        fatal_error(r, "mutex_trylock at %s:%d", __FILE__, __LINE__);
        goto error_out;
    }
    
    // Complain if
    //   1) the backup directory cannot be stat'd (perhaps because it doesn't exist), or
    //   2) The backup directory is not a directory (perhaps because it's a regular file), or
    //   3) the backup directory can not be opened (maybe permissions?), or
    //   4) The backpu directory cannot be readdir'd (who knows what that could be), or
    //   5) The backup directory is not empty (there's a file in there #6542), or
    //   6) The dir cannot be closedir()'d (who knows...)n
    {
        struct stat sbuf;
        r = stat(dest, &sbuf);
        if (r!=0) {
            r = errno;
            backup_error(r, "Problem stat()ing backup directory %s", dest);
            ignore(pthread_mutex_unlock(&m_mutex)); // don't worry about errors here.
            goto error_out;
        }
        if (!S_ISDIR(sbuf.st_mode)) {
            r = EINVAL;
            backup_error(EINVAL,"Backup destination %s is not a directory", dest);
            ignore(pthread_mutex_unlock(&m_mutex)); // don't worry about errors here.
            goto error_out;
        }
        {
            DIR *dir = opendir(dest);
            if (dir == NULL) {
                r = errno;
                backup_error(r, "Problem opening backup directory %s", dest);
                goto unlock_out;
            }
            errno = 0;
      again:
            struct dirent const *e = readdir(dir);
            if (e!=NULL &&
                (strcmp(e->d_name, ".")==0 ||
                 strcmp(e->d_name, "..")==0)) {
                goto again;
            } else if (e!=NULL) {
                // That's bad.  The directory should be empty.
                r = EINVAL;
                backup_error(r,  "Backup directory %s is not empty", dest);
                closedir(dir);
                goto unlock_out;
            } else if (errno!=0) {
                r = errno;
                backup_error(r, "Problem readdir()ing backup directory %s", dest);
                closedir(dir);
                goto unlock_out;
            } else {
                // e==NULL and errno==0 so that's good.  There are no files, except . and ..
                r = closedir(dir);
                if (r!=0) {
                    r = errno;
                    backup_error(r, "Problem closedir()ing backup directory %s", dest);
                    // The dir is already as closed as I can get it.  Don't call closedir again.
                    goto unlock_out;
                }
            }
        }
    }

    r = pthread_rwlock_wrlock(&m_session_rwlock);
    if (r!=0) {
        fatal_error(r, "Problem obtaining session lock at %s:%d", __FILE__, __LINE__);
        goto unlock_out;
    }

    m_session = new backup_session(source, dest, calls, &m_table, &r);
    print_time("Toku Hot Backup: Started:");    

    r = pthread_rwlock_unlock(&m_session_rwlock);
    if (r!=0) {
        fatal_error(r, "Problem releasing session lock at %s:%d", __FILE__, __LINE__);
        goto unlock_out;
    }
    
    r = this->prepare_directories_for_backup(m_session);
    if (r != 0) {
        goto disable_out;
    }

    this->enable_capture();

    WHEN_GLASSBOX( ({
                m_is_capturing = true;
                m_done_copying = false;
                while (!m_start_copying) sched_yield();
            }) );

    r = m_session->do_copy();
    if (r != 0) {
        // This means we couldn't start the copy thread (ex: pthread error).
        goto disable_out;
    }

disable_out: // preserves r if r!=0
    WHEN_GLASSBOX( ({
        m_done_copying = true;
        // If the client asked us to keep capturing till they tell us to stop, then do what they said.
        while (m_keep_capturing) sched_yield();
            }) );

    m_backup_is_running = false;

    this->disable_capture();

    this->disable_descriptions();

    WHEN_GLASSBOX(m_is_capturing = false);

unlock_out: // preserves r if r!0

    {
        int r2 = pthread_rwlock_wrlock(&m_session_rwlock);
        if (r==0 && r2!=0) {
            fatal_error(r2, "Problem obtaining session lock at %s:%d", __FILE__, __LINE__);
            r = r2;
        }
    }

    print_time("Toku Hot Backup: Finished:");
    delete m_session;
    m_session = NULL;

    {
        int r2 = pthread_rwlock_unlock(&m_session_rwlock);
        if (r==0 && r2!=0) {
            fatal_error(r2, "Problem releasing session lock at %s:%d", __FILE__, __LINE__);
            r = r2;
        }
    }
    {
        int r2 = pthread_mutex_unlock(&m_mutex);
        if (r==0 && r2!=0) {
            fatal_error(r2, "pthread_mutex_unlock failed at %s:%d", __FILE__, __LINE__);
            r = r2;
        }
    }
    if (m_an_error_happened) {
        calls->report_error(m_errnum, m_errstring);
        if (r==0) {
            r = m_errnum; // if we already got an error then keep it.
        }
    }

error_out:
    return r;
}

///////////////////////////////////////////////////////////////////////////////
//
int manager::prepare_directories_for_backup(backup_session *session) {
    int r = 0;
    // Loop through all the current file descriptions and prepare them
    // for backup.
    lock_fmap(); // TODO: #6532 This lock is much too coarse.  Need to refine it.  This lock deals with a race between file->create() and a close() call from the application.  We aren't using the m_refcount in file_description (which we should be) and we even if we did, the following loop would be racy since m_map.size could change while we are running, and file descriptors could come and go in the meanwhile.  So this really must be fixed properly to refine this lock.
    for (int i = 0; i < m_map.size(); ++i) {
        description *file = m_map.get_unlocked(i);
        if (file == NULL) {
            continue;
        }
        
        const char * source_path = file->get_full_source_name();
        if (!session->is_prefix(source_path)) {
            continue;
        }

        char * file_name = session->translate_prefix(source_path);
        file->prepare_for_backup(file_name);
        r = open_path(file_name);
        free(file_name);
        if (r != 0) {
            session->abort();
            backup_error(r, "Failed to open path");
            goto out;
        }

        r = file->create();
        if (r != 0) {
            session->abort();
            backup_error(r, "Could not create backup file.");
            goto out;
        }
    }
    
out:
    unlock_fmap();
    return r;
}

///////////////////////////////////////////////////////////////////////////////
//
void manager::disable_descriptions(void)
{
    printf("disabling\n");
    lock_fmap();
    const int size = m_map.size();
    const int middle __attribute__((unused)) = size / 2; // used only in glassbox mode.
    for (int i = 0; i < size; ++i) {
        WHEN_GLASSBOX( ({
                    if (middle == i) {
                        TRACE("Pausing on i = ", i); 
                        while (m_pause_disable) { sched_yield(); }
                        TRACE("Done Pausing on i = ", i);
                    }
                }) );
        description *file = m_map.get_unlocked(i);
        if (file == NULL) {
            continue;
        }
        
        file->disable_from_backup();
        //printf("sleeping\n"); usleep(1000);
    }
    unlock_fmap();
}

///////////////////////////////////////////////////////////////////////////////
//
// create() -
//
// Description:
//
//     TBD: How is create different from open?  Is the only
// difference that we KNOW the file doesn't yet exist (from
// copy) for the create case?
//
int manager::create(int fd, const char *file) 
{
    TRACE("entering create() with fd = ", fd);
    description *description;
    {
        int r = m_map.put(fd, &description);
        if (r != 0) return r; // The error has been reported.
    }

    char *full_source_file_path = realpath(file, NULL);
    if (full_source_file_path == NULL) {
        int error = errno;
        return error;
    }

    // Add description to hash table.
    {
        int r = m_table.lock();
        if (r!=0) {
            free(full_source_file_path);
            return r;
        }
    }

    source_file *source = NULL;

    {
        source = m_table.get(full_source_file_path);
        if (source == NULL) {
            TRACE("Creating new source file in hash map ", fd);
            source = new source_file(full_source_file_path);
            source->add_reference();
            int r = source->init();
            if (r != 0) {
                // The error has been reported.
                source->remove_reference();
                delete source;
                ignore(m_table.unlock());
                free(full_source_file_path);
                return r;
            }

            m_table.put(source);
        }
    }

    free(full_source_file_path);

    source->add_reference();
    description->set_source_file(source);
    
    {
        int r = m_table.unlock();
        if (r!=0) {
            return r;
        }
    }
    
    {
        int r = pthread_rwlock_rdlock(&m_session_rwlock);
        if (r!=0) {
            the_manager.fatal_error(r, "Trying to lock mutex at %s:%d", __FILE__, __LINE__);
            return r;
        }
    }
            
    
    if (m_session != NULL) {
        {
            int r = source->name_read_lock();
            if (r != 0) {
                ignore(pthread_rwlock_unlock(&m_session_rwlock));
                the_manager.fatal_error(r, "pthread error.");
                return r;
            }
        }
        
        char *backup_file_name;
        {
            int r = m_session->capture_create(source->name(), &backup_file_name);
            if (r!=0) {
                ignore(pthread_rwlock_unlock(&m_session_rwlock));
                return r;
            }
        }
        if (backup_file_name != NULL) {
            description->prepare_for_backup(backup_file_name);
            int r = description->create(); // TODO. create is messy since it doens't report the error, and what constitues anerror may depend on context.  Clean this up.
            if (r != 0) {
                backup_error(r, "Could not create backup file %s", backup_file_name);
                free(backup_file_name);
                ignore(pthread_rwlock_unlock(&m_session_rwlock));
                return r;
            }
            free((void*)backup_file_name);
        }
        {
            int r = source->name_unlock();
            if (r != 0) {
                ignore(pthread_rwlock_unlock(&m_session_rwlock));
                the_manager.fatal_error(r, "pthread error.");
                return r;
            }
        }
    }
    {
        int r = pthread_rwlock_unlock(&m_session_rwlock);
        if (r!=0) {
            the_manager.fatal_error(r, "Trying to unlock mutex at %s:%d", __FILE__, __LINE__);
            return r;
        }
    }
    return 0;
}


///////////////////////////////////////////////////////////////////////////////
//
// open() -
//
// Description: 
//
//     If the given file is in our source directory, this method
// creates a new description object and opens the file in
// the backup directory.  We need the bakcup copy open because
// it may be updated if and when the user updates the original/source
// copy of the file.
//
int manager::open(int fd, const char *file, int oflag __attribute__((unused)))
{
    TRACE("entering open() with fd = ", fd);
    description *description;
    {
        int r = m_map.put(fd, &description);
        if (r!=0) return r; // Unlike some of the other functions, we don't do the orignal open here.
    }
    
    char *full_source_file_path = realpath(file, NULL);
    if (full_source_file_path == NULL) {
        int error = errno;
        return error;
    }

    // Add description to hash table.
    {
        int r = m_table.lock();
        if (r!=0) {
            free(full_source_file_path);
            return r;
        }
    }

    source_file * source = NULL;

    {
        source = m_table.get(full_source_file_path);
        if (source == NULL) {
            TRACE("Creating new source file in hash map ", fd);
            source = new source_file(full_source_file_path);
            source->add_reference();
            int r = source->init();
            if (r != 0) {
                // The error has been reported.
                source->remove_reference();
                delete source;
                ignore(m_table.unlock());
                free(full_source_file_path);
                return r;
            }
            m_table.put(source);
        }
    }
    
    free(full_source_file_path);

    source->add_reference();
    description->set_source_file(source);
    
    {
        int r = m_table.unlock();
        if (r!=0) return r;
    }
    {
        int r = pthread_rwlock_rdlock(&m_session_rwlock);
        if (r!=0) {
            the_manager.fatal_error(r, "Trying to lock mutex at %s:%d", __FILE__, __LINE__);
            return r;
        }
    }

    if(m_session != NULL) {
        {
            int r = source->name_read_lock();
            if (r != 0) {
                ignore(pthread_rwlock_unlock(&m_session_rwlock));
                the_manager.fatal_error(r, "pthread error.");
                return r;
            }
        }
        char *backup_file_name;
        {
            int r = m_session->capture_open(file, &backup_file_name);
            if (r!=0) {
                ignore(pthread_rwlock_unlock(&m_session_rwlock));
                return r;
            }
        }
        if(backup_file_name != NULL) {
            description->prepare_for_backup(backup_file_name);
            int r = description->open(); // TODO: open is messy.  It ought to report the error, but I'm not sure if it can, since what constitutes an error may depend on the context.  Clean this up.
            if (r != 0) {
                backup_error(r, "Could not open backup file %s", backup_file_name);
                free(backup_file_name);
                source->name_unlock();
                ignore(pthread_rwlock_unlock(&m_session_rwlock));
                return r;
            }
            free(backup_file_name);
        }
        {
            int r = source->name_unlock();
            if (r != 0) {
                ignore(pthread_rwlock_unlock(&m_session_rwlock));
                the_manager.fatal_error(r, "pthread error.");
                return r;
            }
        }
    }
    {
        int r = pthread_rwlock_unlock(&m_session_rwlock);
        if (r!=0) {
            the_manager.fatal_error(r, "Trying to unlock mutex at %s:%d", __FILE__, __LINE__);
            return r;
        }
    }
    return 0;
}


///////////////////////////////////////////////////////////////////////////////
//
// close() -
//
// Description:
//
//     Find and deallocate file description based on incoming fd.
//
void manager::close(int fd) {
    TRACE("entering close() with fd = ", fd);
    description * file = NULL;
    int r = m_map.get(fd, &file);
    if (r != 0) {
        the_manager.fatal_error(r, "Pthread locking failure trying to close file.");
        return;
    }
    
    if (file != NULL) {
        source_file * source = file->get_source_file();
        source->remove_reference();
        file->set_source_file(NULL);
    }
    
    int r1 = m_map.erase(fd); // If the fd exists in the map, close it and remove it from the mmap.
    if (r1!=0) {
        return;  // Any errors have been reported.  The caller doesn't care.
    }
    // Any errors have been reported, and the caller doesn't want to hear about them.
}


///////////////////////////////////////////////////////////////////////////////
//
// write() -
//
// Description: 
//
//     Using the given file descriptor, this method updates the 
// backup copy of a prevously opened file.
//     Also does the write itself (the write is in here so that a lock can be obtained to protect the file offset)
//
ssize_t manager::write(int fd, const void *buf, size_t nbyte)
{
    TRACE("entering write() with fd = ", fd);
    bool ok = true;
    description *description;
    if (ok) {
        int r = m_map.get(fd, &description);
        if (r!=0) ok = false;
    }
    bool have_description_lock = false;
    if (ok && description) {
        int r = description->lock();
        if (r!=0) ok = false;
        else      have_description_lock = true;
    }
    source_file *file = 0;
    bool have_range_lock = false;
    uint64_t lock_start=0, lock_end=0;
    if (ok && description) {
        {
            int r = m_table.lock();
            if (r!=0) ok = false;
        }
        if (ok) {
            file = m_table.get(description->get_full_source_name());
            int r = m_table.unlock();
            if (r!=0) ok = false;
        }
        if (ok) {
          // We need the range lock before calling real lock so that the write into the source and backup are atomic wrt other writes.
          TRACE("Grabbing file range lock() with fd = ", fd);
          lock_start = description->get_offset();
          lock_end   = lock_start + nbyte;

          // We want to release the description->lock ASAP, since it's limiting other writes.
          // We cannot release it before the real write since the real write determines the new m_offset.
          {
              int r = file->lock_range(lock_start, lock_end);
              if (r!=0) ok = false;
              else      have_range_lock = true;
          }
       }
    }
    ssize_t n_wrote = call_real_write(fd, buf, nbyte);
    if (n_wrote>0 && description) { // Don't need OK, just need description
        // actually wrote something.
        description->increment_offset(n_wrote);
    }
    // Now we can release the description lock, since the offset is calculated.  Release it even if not OK.
    if (have_description_lock) {
        int r = description->unlock();
        if (r != 0) ok=false;
    }

    // We still have the lock range, with which we do the pwrite.
    if (ok && capture_is_enabled()) {
        TRACE("write() captured with fd = ", fd);
        int r = description->pwrite(buf, nbyte, lock_start);
        if (r!=0) {
            // The error has been reported.
            ok = false;
        }
    }
    if (have_range_lock) { // Release the range lock if if not OK.
        TRACE("Releasing file range lock() with fd = ", fd);
        int r = file->unlock_range(lock_start, lock_end);
        // The error has been reported.
        if (r!=0) ok = false;
    }
    return n_wrote;
}

///////////////////////////////////////////////////////////////////////////////
//
// read() -
//
// Description:
//
//     Do the read.
//
ssize_t manager::read(int fd, void *buf, size_t nbyte) {
    TRACE("entering write() with fd = ", fd);
    ssize_t r = 0;
    description *description;
    int rr = m_map.get(fd, &description);
    if (rr!=0 || description == NULL) {
        r = call_real_read(fd, buf, nbyte);
    } else {
        {
            int rrr = description->lock();
            if (rrr!=0) {
                // Do the read if there was an error.
                return call_real_read(fd, buf, nbyte);
            }
        }
        r = call_real_read(fd, buf, nbyte);
        if (r>0) {
            description->increment_offset(r); //moves the offset
        }
        {
            int rrr = description->unlock();
            if (rrr!=0) {
                backup_error(rrr, "failed unlock at %s:%d", __FILE__, __LINE__);
            }
        }
    }
    
    return r;
}

///////////////////////////////////////////////////////////////////////////////
//
// pwrite() -
//
// Description:
//
//     Same as regular write, but uses additional offset argument
// to write to a particular position in the backup file.
//
ssize_t manager::pwrite(int fd, const void *buf, size_t nbyte, off_t offset)
// Do the write, returning the number of bytes written.
// Note: If the backup destination gets a short write, that's an error.
{
    TRACE("entering pwrite() with fd = ", fd);
    description *description;
    {
        int r = m_map.get(fd, &description);
        if (r!=0 || description == NULL) {
            return call_real_pwrite(fd, buf, nbyte, offset);
        }
    }

    {
        int r = m_table.lock();
        if (r!=0) return call_real_pwrite(fd, buf, nbyte, offset);
    }
    source_file * file = m_table.get(description->get_full_source_name());
    {
        int r = m_table.unlock();
        if (r!=0) return call_real_pwrite(fd, buf, nbyte, offset);
    }
    {
        int r = file->lock_range(offset, offset+nbyte);
        if (r!=0) return call_real_pwrite(fd, buf, nbyte, offset);
    }
    ssize_t nbytes_written = call_real_pwrite(fd, buf, nbyte, offset);
    int e = 0;
    if (nbytes_written>0) {
        if (this->capture_is_enabled()) {
            ignore(description->pwrite(buf, nbyte, offset)); // nothing more to do.  It's been reported.
        }
    } else if (nbytes_written<0) {
        e = errno; // save the errno
    }
    ignore(file->unlock_range(offset, offset+nbyte)); // nothing more to do.  It's been reported.
    if (nbytes_written<0) {
        errno = e; // restore errno
    }
    return nbytes_written;
}


///////////////////////////////////////////////////////////////////////////////
//
// seek() -
//
// Description: 
//
//     Move the backup file descriptor to the new position.  This allows
// upcoming intercepted writes to be backed up properly.
//
off_t manager::lseek(int fd, size_t nbyte, int whence) {
    TRACE("entering seek() with fd = ", fd);
    description *description;
    bool ok = true;
    {
        int r = m_map.get(fd, &description);
        if (r!=0 || description==NULL) ok = false;
    }
    if (ok) {
        int r = description->lock();
        if (r!=0) ok = false;
    }
    off_t new_offset = call_real_lseek(fd, nbyte, whence);
    if (ok) {
        description->lseek(new_offset);
        int r __attribute__((unused)) = description->unlock();
    }
    return new_offset;
}


///////////////////////////////////////////////////////////////////////////////
//
// rename() -
//
// Description:
//
//     TBD...
//
int manager::rename(const char *oldpath, const char *newpath)
{
    int user_error = 0;
    int error = 0;
    const char * full_old_destination_path = NULL;
    const char * full_new_destination_path = NULL;
    const char *full_old_path = realpath(oldpath, NULL);
    if (full_old_path == NULL) {
        error = errno;
        the_manager.backup_error(error, "Could not rename file.");
        return call_real_rename(oldpath, newpath);
    }
    const char *full_new_path = realpath(newpath, NULL);
    if (full_new_path == NULL) {
        error = errno;
        free((void*)full_old_path);
        the_manager.backup_error(error, "Could not rename file.");
        return call_real_rename(oldpath, newpath);;
    }
    
    // Rename the source file using the 'name lock'.
    int r = m_table.rename_locked(full_old_path, full_new_path);
    if (r != 0) {
        error = errno;
        the_manager.fatal_error(error, "pthread error. Could not rename file.");
        user_error = call_real_rename(oldpath, newpath);
        goto source_free_out;
    }

    r = pthread_rwlock_rdlock(&m_session_rwlock);
    if (r!=0) {
        the_manager.fatal_error(r, "Trying to lock mutex at %s:%d", __FILE__, __LINE__);
        user_error = call_real_rename(oldpath, newpath);
        goto source_free_out;
    }
    
    // If backup is running...
    if (m_session != NULL && this->capture_is_enabled()) {
        // Check to see that both paths are in our source directory.
        if (!m_session->is_prefix(full_old_path) || !m_session->is_prefix(full_new_path)) {
            user_error = call_real_rename(oldpath, newpath);
            goto source_free_out;
        }
        
        full_old_destination_path = m_session->translate_prefix(full_old_path);
        full_new_destination_path = m_session->translate_prefix(full_new_path);
        
        r = m_table.lock();
        if (r != 0) {
            user_error = call_real_rename(oldpath, newpath);
            goto destination_free_out;
        }
        
        source_file * file = m_table.get(full_new_path);
        file->add_reference();
        r = m_table.unlock();
        if (r != 0) {
            user_error = call_real_rename(oldpath, newpath);
            file->remove_reference();
            goto destination_free_out;
        }
        
        user_error = call_real_rename(oldpath, newpath);
        if (user_error == 0) {
            //
            // Rename the backup version, if it exists.
            //
            // If the copier has already copied or is copying the file, this
            // will succceed.  If the copier has not yet created the file
            // this will fail, and it should find it in its todo list.
            // However, if we want to be sure that the new name is in its todo
            // list, we must add it to the todo list ourselves, just to be sure
            // that it is copied.
            //
            // NOTE: If the orignal file name is still in our todo list, the
            // copier will attempt to copy it, but since it has already been
            // renamed it will fail with ENOENT, which we ignore in COPY, and 
            // move on to the next item in the todo list. 
            //
            r = call_real_rename(full_old_destination_path, full_new_destination_path);
            if (r != 0) {
                error = errno;
                if (error != ENOENT) {
                    the_manager.backup_error(error, "rename() on backup copy failed.");
                } else {
                    // Add to the copier's todo list.
                    m_session->add_to_copy_todo_list(full_new_destination_path);
                }
            }
        }
        

    } else {
        // Backup is not running.  Just call the syscall on the source file.
        user_error = call_real_rename(oldpath, newpath);
    }
    
    r = pthread_rwlock_unlock(&m_session_rwlock);
    if (r != 0) {
        the_manager.fatal_error(r, "Trying to lock mutex at %s:%d", __FILE__, __LINE__);
    }

destination_free_out:
    if (full_old_destination_path != NULL) {
        free((void*)full_old_destination_path);
    }
    if (full_new_destination_path != NULL) {
        free((void*)full_new_destination_path);
    }

source_free_out:
    free((void*)full_old_path);
    free((void*)full_new_path);
    return user_error;
}

///////////////////////////////////////////////////////////////////////////////
//
// unlink() -
//
//
int manager::unlink(const char *path)
{
    int user_error = 0;
    const char * full_path = realpath(path, NULL);
    if (full_path == NULL) {
        int error = errno;
        user_error = call_real_unlink(path);
        the_manager.backup_error(error, "Could not unlink path.");
        return user_error;
    }

    // Get file/dir from hash table.
    source_file * file = NULL;
    {
        int r = m_table.lock();
        if (r != 0) {
            user_error = call_real_unlink(path);
            goto free_out;
        }
    }

    file = m_table.get(path);
    user_error = call_real_unlink(path);
    
    if (this->capture_is_enabled()) {
        // If it does not exist, and if backup is running,
        // it may be in the todo list. Since we have the hash 
        // table lock, the copier can't add it, and rename() threads
        // can't alter the name and re-hash it till we are done.
        char * dest_name = m_session->translate_prefix_of_realpath(full_path);
        int r = call_real_unlink(dest_name);
        int error = 0;
        if (r!=0) error=errno;
        free(dest_name);
        if (r != 0) {
            if (error != ENOENT) {
                the_manager.backup_error(error, "Could not unlink backup copy.");
                goto unlock_out;
            }
        }
    }
        
    if (file) m_table.try_to_remove(file);

unlock_out:
    {
        int r = m_table.unlock();
        if (r != 0) {
            // TODO.  If this fails, then we should do something.
            goto free_out;
        }
    }

free_out:
    free((void*)full_path);
    return user_error;
}

///////////////////////////////////////////////////////////////////////////////
//
// ftruncate() -
//
// Description:
//
//     TBD...
//
int manager::ftruncate(int fd, off_t length)
{
    TRACE("entering ftruncate with fd = ", fd);
    // TODO: Remove the logic for null descriptions, since we will
    // always have a description and a source_file.
    description *description;
    {
        int r = m_map.get(fd, &description);
        if (r!=0 || description == NULL) {
            return call_real_ftruncate(fd, length);
        }
    }

    {
        int r = m_table.lock();
        if (r!=0) return call_real_ftruncate(fd, length);
    }
    source_file * file = m_table.get(description->get_full_source_name());
    {
        int r = m_table.unlock();
        if (r!=0) return call_real_ftruncate(fd, length);
    }
    {
        int r = file->lock_range(length, LLONG_MAX);
        if (r!=0) return call_real_ftruncate(fd, length);
    }
    int user_result = call_real_ftruncate(fd, length);
    int e = 0;
    if (user_result==0) {
        if (this->capture_is_enabled()) {
            ignore(description->truncate(length)); // it's been reported, so there's nothing we can do about that error except to try to unlock the range.
        }
    } else {
        e = errno; // save errno
    }
    ignore(file->unlock_range(length, LLONG_MAX)); // it's been reported, so there's not much more to do
    if (user_result!=0) {
        errno = e; // restore errno
    }
    return user_result;
}

///////////////////////////////////////////////////////////////////////////////
//
// truncate() -
//
// Description:
//
//     TBD...
//
int manager::truncate(const char *path, off_t length)
{
    int user_error = 0;
    int error = 0;
    const char * full_path = realpath(path, NULL);
    if (full_path == NULL) {
        error = errno;
        the_manager.backup_error(error, "Failed to truncate backup file.");
        return call_real_truncate(path, length);
    }

    int r = pthread_rwlock_rdlock(&m_session_rwlock);
    if (r != 0) {
        the_manager.fatal_error(r, "Trying to lock mutex at %s:%d", __FILE__, __LINE__);
        user_error = call_real_truncate(path, length);
        goto free_out;
    }

    
    if (m_session != NULL && m_session->is_prefix(full_path)) {
        const char * destination_file = NULL;
        destination_file = m_session->translate_prefix(full_path);
        // Find and lock the associated source file.
        r = m_table.lock();
        if (r != 0) {
            user_error = call_real_truncate(path, length);
            goto free_out;
        }

        source_file * file = m_table.get(destination_file);
        file->add_reference();
        r = m_table.unlock();
        if (r != 0) {
            user_error = call_real_truncate(path, length);
            file->remove_reference();
            goto free_out;
        }
        
        r = file->lock_range(length, LLONG_MAX);
        if (r != 0) {
            user_error = call_real_truncate(path, length);
            file->remove_reference();
            goto free_out;
        }
        
        user_error = call_real_truncate(full_path, length);
        if (user_error == 0 && this->capture_is_enabled()) {
            r = call_real_truncate(destination_file, length);
            if (r != 0) {
                error = errno;
                if (error != ENOENT) {
                    the_manager.backup_error(error, "Could not truncate backup file.");
                }
            }
        }

        r = file->unlock_range(length, LLONG_MAX);
        file->remove_reference();
        if (r != 0) {
            user_error = call_real_truncate(path, length);
            goto free_out;
        }
        
        free((void*) destination_file);
    } else {
        // No backup is in progress, just truncate the source file.
        user_error = call_real_truncate(path, length);
        if (user_error != 0) {
            goto free_out;
        }
    }

    r = pthread_rwlock_unlock(&m_session_rwlock);
    if (r != 0) {
        the_manager.fatal_error(r, "Trying to lock mutex at %s:%d", __FILE__, __LINE__);
    }
    
free_out:
    free((void*) full_path);
    return user_error;

}

///////////////////////////////////////////////////////////////////////////////
//
// mkdir() -
//
// Description:
//
//     TBD...
//
void manager::mkdir(const char *pathname)
{
    {
        int r = pthread_rwlock_rdlock(&m_session_rwlock);
        if (r!=0) {
            the_manager.fatal_error(r, "failed releasing rwlock at %s:%d", __FILE__, __LINE__);
            return; // don't try to do any more once the lock failed.
        }
    }

    if(m_session != NULL) {
        int r = m_session->capture_mkdir(pathname);
        if (r != 0) {
            the_manager.backup_error(r, "failed mkdir creating %s", pathname);
            // proceed to unlocking below
        }
    }

    {
        int r = pthread_rwlock_unlock(&m_session_rwlock);
        if (r!=0) {
            the_manager.fatal_error(r, "failed releasing rwlock at %s:%d", __FILE__, __LINE__);
        }
    }
            
}


///////////////////////////////////////////////////////////////////////////////
//
void manager::set_throttle(unsigned long bytes_per_second) {
    VALGRIND_HG_DISABLE_CHECKING(&m_throttle, sizeof(m_throttle));
    m_throttle = bytes_per_second;
}

///////////////////////////////////////////////////////////////////////////////
//
unsigned long manager::get_throttle(void) {
    return m_throttle;
}

///////////////////////////////////////////////////////////////////////////////
//
void manager::fatal_error(int errnum, const char *format_string, ...)
{
    va_list ap;
    va_start(ap, format_string);
    
    this->kill();
    this->disable_capture();
    set_error_internal(errnum, format_string, ap);
    va_end(ap);
}

///////////////////////////////////////////////////////////////////////////////
//
void manager::backup_error(int errnum, const char *format_string, ...) {
    va_list ap;
    va_start(ap, format_string);
    
    this->disable_capture();
    set_error_internal(errnum, format_string, ap);
    va_end(ap);
}

///////////////////////////////////////////////////////////////////////////////
//
void manager::set_error_internal(int errnum, const char *format_string, va_list ap) {
    m_backup_is_running = false;
    {
        int r = pthread_mutex_lock(&m_error_mutex); // don't use pmutex here.
        if (r!=0) {
            this->kill();  // go ahead and set the error, however
        }
    }
    if (!m_an_error_happened) {
        int len = 2*PATH_MAX + strlen(format_string) + 1000; // should be big enough for all our errors
        char *string = (char*)malloc(len);
        int nwrote = vsnprintf(string, len, format_string, ap);
        snprintf(string+nwrote, len-nwrote, "   error %d (%s)", errnum, strerror(errnum));
        if (m_errstring) free(m_errstring);
        m_errstring = string;
        m_errnum = errnum;
        m_an_error_happened = true; // set this last so that it will be OK.
    }
    {
        int r = pthread_mutex_unlock(&m_error_mutex);  // don't use pmutex here.
        if (r!=0) {
            this->kill();
        }
    }
}


#ifdef GLASSBOX
// Test routines.
void manager::pause_disable(bool pause)
{
    VALGRIND_HG_DISABLE_CHECKING(&m_pause_disable, sizeof(m_pause_disable));
    m_pause_disable = pause;
}

void manager::set_keep_capturing(bool keep_capturing) {
    VALGRIND_HG_DISABLE_CHECKING(&m_keep_capturing, sizeof(m_keep_capturing));
    m_keep_capturing = keep_capturing;
}

bool manager::is_done_copying(void) {
    return m_done_copying;
}
bool manager::is_capturing(void) {
    return m_is_capturing;
}

void manager::set_start_copying(bool start_copying) {
    VALGRIND_HG_DISABLE_CHECKING(&m_start_copying, sizeof(m_start_copying));
    m_start_copying = start_copying;
}
#endif /*GLASSBOX*/