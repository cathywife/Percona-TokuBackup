/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:  
#ident "Copyright (c) 2012-2013 Tokutek Inc.  All rights reserved."
#ident "$Id$"

#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <vector>
#include <valgrind/valgrind.h>
#include <algorithm>

#include "backup_test_helpers.h"

const int N=10;
int N_BACKUP_ITERATIONS = 100;
int N_OP_ITERATIONS = 1000000;
const int N_FNAMES = 8;

volatile long counter = N+1;

void* do_backups(void *v) {
    check(v==NULL);
    for (int i=0; i<N_BACKUP_ITERATIONS; i++) {
        setup_destination();
        pthread_t thread;
        start_backup_thread(&thread);
        finish_backup_thread(thread);
    }
    __sync_fetch_and_add(&counter, -1);
    while (counter>0) {
        pthread_t thread;
        start_backup_thread(&thread);
        finish_backup_thread(thread);
        sched_yield();
    }
    return v;
}

volatile int open_results[2];

char *fnames[N_FNAMES];

void do_client_once(std::vector<int> *fds) {
    const int casesize = 2;
    switch(random()%casesize) {
        case 0: {
            char *name = fnames[random()%N_FNAMES];
            int excl_flags = random()%2 ? O_EXCL : 0;
            int creat_flags = random()%2 ? O_CREAT : 0;
            int fd = open(name, excl_flags | O_RDWR | creat_flags, 0777);
            if (fd>=0) {
                fds->push_back(fd);
            }
            __sync_fetch_and_add(&open_results[fd>=0 ? 0 : 1], 1);
            break;
        }
        case 1: {
            if (fds->size()>0) {
                size_t idx = random() % fds->size();
                int fd = (*fds)[idx];
                int endfd = (*fds)[fds->size()-1];
                (*fds)[idx] = endfd;
                fds->pop_back();
                int r = close(fd);
                check(r==0);
            }
            break;
        }
    }
}

void* do_client(void *v) {
    int me = *(int*)v;
    check(0<=me && me<N);
    std::vector<int> fds;
    fds.push_back(3);
    check(fds[0]==3);
    fds.pop_back();
    for (int i=0; i<N_OP_ITERATIONS; i++) {
        do_client_once(&fds);
    }
    fprintf(stderr, "Client %d done, doing more work till the others are done (fds.size=%ld)\n", me, fds.size());
    __sync_fetch_and_add(&counter, -1);
    while (counter>0) {
        do_client_once(&fds);
        if (RUNNING_ON_VALGRIND) usleep(10); // do some more resting to ease up on the valgrind time.
    }
    while (fds.size()>0) {
        int fd = fds[fds.size()-1];
        fds.pop_back();
        int r = close(fd);
        check(r==0);
    }
    return v;
}

int test_main(int argc __attribute__((__unused__)), const char *argv[] __attribute__((__unused__))) {
    if (RUNNING_ON_VALGRIND) {
        N_OP_ITERATIONS = std::max(N_OP_ITERATIONS/2000,500); // do less work if we are in valgrind
        N_BACKUP_ITERATIONS = std::max(N_BACKUP_ITERATIONS/10, 10);
    }
    fprintf(stderr, "N_OP_ITERATIONS=%d N_BACKUP_ITERATIONS=%d\n", N_OP_ITERATIONS, N_BACKUP_ITERATIONS);
    setup_source();
    setup_destination();
    char *src = get_src();
    int len = strlen(src)+10;
    for (int i=0; i<N_FNAMES; i++) {
        fnames[i] = (char*)malloc(len);
        int r = snprintf(fnames[i], len, "%s/A", src);
        check(r<len);
    }
    pthread_t backups;
    {
        int r = pthread_create(&backups, NULL, do_backups, NULL);
        check(r==0);
    }
    pthread_t clients[N];
    int       id[N];
    for (int i=0; i<N; i++) {
        id[i] = i;
        int r = pthread_create(&clients[i], NULL, do_client, &id[i]);
        check(r==0);
    }
    {
        void *v;
        int r = pthread_join(backups, &v);
        check(r==0 && v==NULL);
    }
    for (int i=0; i<N; i++) {
        void *v;
        int r = pthread_join(clients[i], &v);
        check(r==0 && v==(void*)&id[i]);
    }
    cleanup_dirs();
    printf("open    %d ok, %d failed\n", open_results[0],   open_results[1]);
    free(src);
    for (int i=0; i<N_FNAMES; i++) {
        free(fnames[i]);
    }
    return 0;
}

template class std::vector<int>;