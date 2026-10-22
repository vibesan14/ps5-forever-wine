/* Standalone host regressions for the helpers used by the native-title
 * loader, ntdll thread registry and descriptor-relative file operations.
 * Build: cc -std=c11 -pthread -Iinclude loader/tests/ps5_title.c -o ps5_title_test
 */
#define _GNU_SOURCE 1
#define WINE_PS5_TITLE_HOST_TEST 1

#include <assert.h>
#include <errno.h>
#include <time.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>

/* Host stand-in for Wine's Unix fault guard: the macros mirror
 * include/wine/unixlib.h, and fault_handler does what handle_syscall_fault
 * does when a jump buffer is armed. */
#define WINE_UNIX_LIB 1

/* The signal handler observes this guard asynchronously; keep its stores
 * observable when the standalone host model is optimized. */
static _Thread_local void *volatile fault_jmp;

static void ntdll_set_exception_jmp_buf( jmp_buf jmp )
{
    assert( !jmp || !fault_jmp );
    fault_jmp = jmp;
}

#define __TRY \
    do { jmp_buf __jmp; \
         int __first = 1; \
         for (;;) if (!__first) \
         { \
             do {

#define __EXCEPT \
             } while(0); \
             ntdll_set_exception_jmp_buf( NULL ); \
             break; \
         } else { \
             if (setjmp( __jmp )) { \
                 do {

#define __ENDTRY \
                 } while (0); \
                 break; \
             } \
             ntdll_set_exception_jmp_buf( __jmp ); \
             __first = 0; \
         } \
    } while (0);

#include "wine/ps5_title_private.h"
#include "wine/ps5_title.h"
#include "wine/ps5_suspend.h"
#include "wine/ps5_desktop.h"

static void test_cursor_message(void)
{
    union
    {
        struct ps5_desktop_cursor_shape shape;
        unsigned char bytes[PS5_DESKTOP_MAX_MESSAGE];
    } message;
    struct ps5_desktop_cursor_shape *shape = &message.shape;
    unsigned char unaligned[PS5_DESKTOP_MAX_MESSAGE + 1];
    _Static_assert( sizeof(*shape) == 40 && offsetof(struct ps5_desktop_cursor_shape, pixels) == 40,
                    "cursor wire prefix" );
    _Static_assert( PS5_DESKTOP_CURSOR_SHAPE == 12, "append-only desktop message ids" );
    *shape = (struct ps5_desktop_cursor_shape){
        .header = {PS5_DESKTOP_CURSOR_SHAPE, PS5_DESKTOP_MAX_MESSAGE}, .id = 100,
        .width = 64, .height = 64, .hotspot_x = 63, .hotspot_y = 63, .visible = 1};
    memset( shape->pixels, 0xff, 64 * 64 * sizeof(uint32_t) );
    assert( ps5_desktop_cursor_shape_valid( shape, PS5_DESKTOP_MAX_MESSAGE ));
    memcpy( unaligned + 1, shape, PS5_DESKTOP_MAX_MESSAGE );
    assert( ps5_desktop_cursor_shape_valid( unaligned + 1, PS5_DESKTOP_MAX_MESSAGE ));
    assert( !ps5_desktop_cursor_shape_valid( shape, sizeof(*shape) - 1 ));
    assert( !ps5_desktop_cursor_shape_valid( shape, PS5_DESKTOP_MAX_MESSAGE - 4 ));
    shape->width = UINT32_MAX;
    assert( !ps5_desktop_cursor_shape_valid( shape, PS5_DESKTOP_MAX_MESSAGE ));
    shape->width = 64;
    shape->hotspot_y = 64;
    assert( !ps5_desktop_cursor_shape_valid( shape, PS5_DESKTOP_MAX_MESSAGE ));
    shape->hotspot_y = -1;
    assert( !ps5_desktop_cursor_shape_valid( shape, PS5_DESKTOP_MAX_MESSAGE ));
    shape->hotspot_y = 0;
    shape->visible = 2;
    assert( !ps5_desktop_cursor_shape_valid( shape, PS5_DESKTOP_MAX_MESSAGE ));
    *shape = (struct ps5_desktop_cursor_shape){.header = {PS5_DESKTOP_CURSOR_SHAPE, sizeof(*shape)}};
    assert( ps5_desktop_cursor_shape_valid( shape, sizeof(*shape) ));
    shape->reserved = 1;
    assert( !ps5_desktop_cursor_shape_valid( shape, sizeof(*shape) ));
    shape->reserved = 0;
    shape->id = PS5_DESKTOP_Z_BOTTOM;
    assert( !ps5_desktop_cursor_shape_valid( shape, sizeof(*shape) ));
    shape->id = 100;
    assert( ps5_desktop_cursor_shape_valid( shape, sizeof(*shape) )); /* per-window hide */
    shape->width = 1;
    assert( !ps5_desktop_cursor_shape_valid( shape, sizeof(*shape) ));
}

static struct wine_ps5_title_threads threads = WINE_PS5_TITLE_THREADS_INIT;
static pthread_key_t thread_key;

struct signal_test
{
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    struct wine_ps5_title_thread entry;
    const void *teb;
    unsigned int id;
    int ready, signalled, finish;
};

static void unregister_thread( void *entry )
{
    wine_ps5_title_unregister_thread( &threads, entry );
}

static int fail_specific( pthread_key_t key, const void *value )
{
    return EAGAIN;
}

static void *signal_client( void *arg )
{
    struct signal_test *test = arg;
    sigset_t signals;
    int sig;

    sigemptyset( &signals );
    sigaddset( &signals, SIGUSR1 );
    assert( !wine_ps5_title_register_current_thread( &threads, &test->entry, test->id, test->teb,
                                                    thread_key, pthread_setspecific ));
    pthread_mutex_lock( &test->mutex );
    test->ready = 1;
    pthread_cond_signal( &test->condition );
    pthread_mutex_unlock( &test->mutex );
    assert( !sigwait( &signals, &sig ));
    assert( sig == SIGUSR1 );
    pthread_mutex_lock( &test->mutex );
    test->signalled = 1;
    pthread_cond_signal( &test->condition );
    while (!test->finish) pthread_cond_wait( &test->condition, &test->mutex );
    pthread_mutex_unlock( &test->mutex );
    return NULL;  /* native TLS destructor unregisters before pthread/TEB reuse */
}

static void *concurrent_lookup( void *arg )
{
    struct signal_test *test = arg;
    unsigned int i;
    int ret;

    /* Bounded race exercise, not a wait/polling loop. Every call is valid
     * either before or after the client's exit destructor wins the mutex. */
    for (i = 0; i < 4096; i++)
    {
        ret = wine_ps5_title_signal_thread( &threads, test->id, test->teb, 0 );
        assert( !ret || (ret == -1 && errno == ESRCH) );
    }
    return NULL;
}

static void test_thread_registry(void)
{
    pthread_mutex_t virtual_mutex = PTHREAD_MUTEX_INITIALIZER;
    struct signal_test test = { .mutex = PTHREAD_MUTEX_INITIALIZER,
                                .condition = PTHREAD_COND_INITIALIZER, .id = 101 };
    struct wine_ps5_title_thread failed = {0}, duplicate = {0}, reused = {0};
    sigset_t signals, previous;
    pthread_t client, lookup;
    size_t page = sysconf( _SC_PAGESIZE );
    void *teb;

    /* An unreadable opaque TEB key makes accidental TEB dereference fail. */
    teb = mmap( NULL, page, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0 );
    assert( teb != MAP_FAILED );
    test.teb = teb;
    assert( !pthread_key_create( &thread_key, unregister_thread ));
    assert( wine_ps5_title_signal_thread( &threads, test.id, teb, 0 ) == -1 && errno == ESRCH );
    assert( wine_ps5_title_register_current_thread( &threads, &failed, test.id, teb,
                                                   thread_key, fail_specific ) == EAGAIN );
    assert( wine_ps5_title_signal_thread( &threads, test.id, teb, 0 ) == -1 && errno == ESRCH );
    assert( threads.head == NULL );
    sigemptyset( &signals );
    sigaddset( &signals, SIGUSR1 );
    assert( !pthread_sigmask( SIG_BLOCK, &signals, &previous ));
    assert( !pthread_create( &client, NULL, signal_client, &test ));
    pthread_mutex_lock( &test.mutex );
    while (!test.ready) pthread_cond_wait( &test.condition, &test.mutex );
    pthread_mutex_unlock( &test.mutex );
    /* The server may recycle a Windows ID before the dead native thread has
     * completed its exit destructor. The opaque TEB still disambiguates it. */
    assert( !wine_ps5_title_register_thread( &threads, &duplicate, test.id, (char *)teb + 1,
                                             pthread_self() ));
    assert( wine_ps5_title_register_thread( &threads, &duplicate, test.id + 1,
                                            (char *)teb + 2, pthread_self() ) == EEXIST );
    assert( !wine_ps5_title_signal_thread( &threads, test.id, (char *)teb + 1, 0 ));
    wine_ps5_title_unregister_thread( &threads, &duplicate );
    assert( wine_ps5_title_register_thread( &threads, &duplicate, test.id + 1, teb,
                                           pthread_self() ) == EEXIST );
    /* A client owns virtual_mutex while waiting synchronously for a signal
     * dispatch and acknowledgement. Lookup must not acquire that mutex. */
    pthread_mutex_lock( &virtual_mutex );
    assert( !wine_ps5_title_signal_thread( &threads, test.id, teb, SIGUSR1 ));
    pthread_mutex_lock( &test.mutex );
    while (!test.signalled) pthread_cond_wait( &test.condition, &test.mutex );
    pthread_mutex_unlock( &test.mutex );
    pthread_mutex_unlock( &virtual_mutex );
    assert( !pthread_create( &lookup, NULL, concurrent_lookup, &test ));
    pthread_mutex_lock( &test.mutex );
    test.finish = 1;
    pthread_cond_signal( &test.condition );
    pthread_mutex_unlock( &test.mutex );
    assert( !pthread_join( client, NULL ));
    assert( !pthread_join( lookup, NULL ));
    assert( threads.head == NULL );
    assert( !munmap( teb, page ));
    assert( wine_ps5_title_signal_thread( &threads, test.id, teb, 0 ) == -1 && errno == ESRCH );
    /* Reuse exactly the TEB address, but not the dead Windows thread ID. */
    assert( mmap( teb, page, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0 ) == teb );
    assert( !wine_ps5_title_register_thread( &threads, &reused, test.id + 1, teb, pthread_self() ));
    assert( wine_ps5_title_signal_thread( &threads, test.id, teb, 0 ) == -1 && errno == ESRCH );
    assert( !wine_ps5_title_signal_thread( &threads, test.id + 1, teb, 0 ));
    wine_ps5_title_unregister_thread( &threads, &reused );
    wine_ps5_title_unregister_thread( &threads, &reused );
    assert( threads.head == NULL );
    assert( !munmap( teb, page ));
    assert( !pthread_key_delete( thread_key ));
    assert( !pthread_sigmask( SIG_SETMASK, &previous, NULL ));
    pthread_mutex_destroy( &virtual_mutex );
    pthread_mutex_destroy( &test.mutex );
    pthread_cond_destroy( &test.condition );
}

struct exit_test
{
    struct wine_ps5_title_exit state;
    pthread_t owner, server;
    int control[2];
    int fatal, joined;
};

static struct exit_test *shutdown_test;

static void *initial_windows_thread( void *arg )
{
    return NULL;  /* ExitThread with workers remaining must not publish exit */
}

static void *private_server( void *arg )
{
    struct exit_test *test = arg;
    char byte;

    if (test->fatal)
    {
        wine_ps5_title_request_exit( &test->state, 1 );
        pthread_exit( NULL );
    }
    assert( read( test->control[1], &byte, 1 ) == 0 );
    return NULL;
}

static void *worker_process_exit( void *arg )
{
    struct exit_test *test = arg;

    wine_ps5_title_request_exit( &test->state, 37 );
    pthread_exit( NULL );
}

static void stop_private_server(void)
{
    struct exit_test *test = shutdown_test;

    assert( pthread_equal( pthread_self(), test->owner ));
    assert( test->joined == 0 );
    /* A late/concurrent fatal request cannot replace the first status or
     * deadlock with the supervisor while it joins the status producer. */
    wine_ps5_title_request_exit( &test->state, 99 );
    assert( !shutdown( test->control[0], SHUT_RDWR ));
    assert( !pthread_join( test->server, NULL ));
    test->joined++;
}

static void test_supervisor( int fatal )
{
    struct exit_test test = { .state = WINE_PS5_TITLE_EXIT_INIT, .fatal = fatal };
    pthread_t initial, worker;
    int status;

    test.owner = pthread_self();
    shutdown_test = &test;
    assert( !socketpair( AF_UNIX, SOCK_STREAM, 0, test.control ));
    assert( !pthread_create( &initial, NULL, initial_windows_thread, &test ));
    assert( !pthread_join( initial, NULL ));
    assert( test.state.requested == 0 );
    assert( !pthread_create( &test.server, NULL, private_server, &test ));
    if (!fatal)
    {
        assert( !pthread_create( &worker, NULL, worker_process_exit, &test ));
        /* The exit requester finishes even while the server is blocked. */
        assert( !pthread_join( worker, NULL ));
    }
    status = wine_ps5_title_supervise_exit( &test.state, stop_private_server );
    assert( status == (fatal ? 1 : 37) );
    assert( test.joined == 1 );
    assert( test.state.status == status );
    close( test.control[0] );
    close( test.control[1] );
    pthread_mutex_destroy( &test.state.mutex );
    pthread_cond_destroy( &test.state.condition );
}

struct callback_test
{
    struct wine_ps5_title_exit exit;
    struct wine_ps5_title_callbacks callbacks;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int published, allow_return, inside_loader, closed;
};

static void paused_loader_callback( struct callback_test *test )
{
    pthread_mutex_lock( &test->mutex );
    test->inside_loader = 1;
    pthread_mutex_unlock( &test->mutex );
    wine_ps5_title_request_exit( &test->exit, 37 );
    pthread_mutex_lock( &test->mutex );
    test->published = 1;
    pthread_cond_broadcast( &test->condition );
    while (!test->allow_return) pthread_cond_wait( &test->condition, &test->mutex );
    pthread_mutex_unlock( &test->mutex );
}

static void *callback_client( void *arg )
{
    struct callback_test *test = arg;
    sigset_t blocked, previous;

    wine_ps5_title_critical_signals( &blocked );
    pthread_sigmask( SIG_BLOCK, &blocked, &previous );
    assert( wine_ps5_title_enter_callback( &test->callbacks ));
    paused_loader_callback( test );
    /* This acknowledgement executes only after returning out of loader code. */
    pthread_mutex_lock( &test->mutex );
    test->inside_loader = 0;
    pthread_mutex_unlock( &test->mutex );
    wine_ps5_title_leave_callback( &test->callbacks );
    pthread_sigmask( SIG_SETMASK, &previous, NULL );
    return NULL;
}

static void *close_loader_callbacks( void *arg )
{
    struct callback_test *test = arg;

    wine_ps5_title_close_callbacks( &test->callbacks );
    pthread_mutex_lock( &test->mutex );
    assert( !test->inside_loader );
    test->closed = 1;
    pthread_mutex_unlock( &test->mutex );
    return NULL;
}

static void test_callback_quiescence(void)
{
    struct callback_test test = { .exit = WINE_PS5_TITLE_EXIT_INIT,
                                 .callbacks = WINE_PS5_TITLE_CALLBACKS_INIT,
                                 .mutex = PTHREAD_MUTEX_INITIALIZER,
                                 .condition = PTHREAD_COND_INITIALIZER };
    pthread_t client, closer;

    assert( !pthread_create( &client, NULL, callback_client, &test ));
    assert( wine_ps5_title_wait_exit( &test.exit ) == 37 );
    pthread_mutex_lock( &test.mutex );
    while (!test.published) pthread_cond_wait( &test.condition, &test.mutex );
    pthread_mutex_unlock( &test.mutex );
    assert( !pthread_create( &closer, NULL, close_loader_callbacks, &test ));
    pthread_mutex_lock( &test.callbacks.mutex );
    while (!test.callbacks.closed) pthread_cond_wait( &test.callbacks.condition, &test.callbacks.mutex );
    assert( test.callbacks.active == 1 );
    pthread_mutex_unlock( &test.callbacks.mutex );
    /* The terminal status is visible, but unmapping is still forbidden. New
     * clients cannot enter the callback while the old one is draining. */
    assert( !wine_ps5_title_enter_callback( &test.callbacks ));
    pthread_mutex_lock( &test.mutex );
    assert( !test.closed && test.inside_loader );
    test.allow_return = 1;
    pthread_cond_broadcast( &test.condition );
    pthread_mutex_unlock( &test.mutex );
    assert( !pthread_join( client, NULL ));
    assert( !pthread_join( closer, NULL ));
    assert( test.closed && !test.callbacks.active );
    assert( !wine_ps5_title_enter_callback( &test.callbacks ));
    pthread_mutex_destroy( &test.mutex );
    pthread_cond_destroy( &test.condition );
    pthread_mutex_destroy( &test.exit.mutex );
    pthread_cond_destroy( &test.exit.condition );
    pthread_mutex_destroy( &test.callbacks.mutex );
    pthread_cond_destroy( &test.callbacks.condition );
}

struct initial_join_test
{
    struct wine_ps5_title_join initial;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int control[2], calls, returned;
};

static struct initial_join_test *initial_join_test;

static void *initial_loader_frame( void *arg )
{
    struct initial_join_test *test = arg;
    char byte;

    assert( read( test->control[1], &byte, 1 ) == 0 );
    test->returned = 1;
    return NULL;
}

static int counted_initial_join( pthread_t thread, void **result )
{
    struct initial_join_test *test = initial_join_test;

    pthread_mutex_lock( &test->mutex );
    test->calls++;
    pthread_cond_signal( &test->condition );
    pthread_mutex_unlock( &test->mutex );
    return pthread_join( thread, result );
}

static void *join_initial_client( void *arg )
{
    struct initial_join_test *test = arg;

    assert( !wine_ps5_title_join_once( &test->initial, counted_initial_join ));
    assert( test->returned );
    return NULL;
}

static void test_initial_thread_quiescence(void)
{
    struct initial_join_test test = { .initial = WINE_PS5_TITLE_JOIN_INIT,
                                     .mutex = PTHREAD_MUTEX_INITIALIZER,
                                     .condition = PTHREAD_COND_INITIALIZER };
    pthread_t reaper, supervisor;

    initial_join_test = &test;
    assert( !socketpair( AF_UNIX, SOCK_STREAM, 0, test.control ));
    assert( !pthread_create( &test.initial.thread, NULL, initial_loader_frame, &test ));
    assert( !pthread_create( &reaper, NULL, join_initial_client, &test ));
    pthread_mutex_lock( &test.mutex );
    while (!test.calls) pthread_cond_wait( &test.condition, &test.mutex );
    pthread_mutex_unlock( &test.mutex );
    assert( !pthread_create( &supervisor, NULL, join_initial_client, &test ));
    pthread_mutex_lock( &test.initial.mutex );
    assert( test.initial.joining && !test.initial.joined );
    pthread_mutex_unlock( &test.initial.mutex );
    assert( !shutdown( test.control[0], SHUT_RDWR ));
    assert( !pthread_join( reaper, NULL ));
    assert( !pthread_join( supervisor, NULL ));
    /* An ordinary early ExitThread followed by shutdown must not join the
     * native handle twice, even if that handle can now be reused. */
    assert( !wine_ps5_title_join_once( &test.initial, counted_initial_join ));
    assert( test.calls == 1 && test.returned && test.initial.joined );
    close( test.control[0] );
    close( test.control[1] );
    pthread_mutex_destroy( &test.mutex );
    pthread_cond_destroy( &test.condition );
    pthread_mutex_destroy( &test.initial.mutex );
    pthread_cond_destroy( &test.initial.condition );
}

static int transfer_directory( int fd )
{
    int sockets[2], received;
    union { struct cmsghdr align; char bytes[CMSG_SPACE(sizeof(fd))]; } control;
    struct msghdr message = {0};
    struct cmsghdr *header;
    char byte = 0;
    struct iovec data = { &byte, 1 };

    assert( !socketpair( AF_UNIX, SOCK_STREAM, 0, sockets ));
    memset( &control, 0, sizeof(control) );
    message.msg_iov = &data;
    message.msg_iovlen = 1;
    message.msg_control = control.bytes;
    message.msg_controllen = sizeof(control.bytes);
    header = CMSG_FIRSTHDR( &message );
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(fd));
    memcpy( CMSG_DATA(header), &fd, sizeof(fd) );
    assert( sendmsg( sockets[0], &message, 0 ) == 1 );
    memset( &control, 0, sizeof(control) );
    assert( recvmsg( sockets[1], &message, 0 ) == 1 );
    assert( !(message.msg_flags & MSG_CTRUNC) );
    header = CMSG_FIRSTHDR( &message );
    assert( header && header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS );
    memcpy( &received, CMSG_DATA(header), sizeof(received) );
    close( sockets[0] );
    close( sockets[1] );
    return received;
}

static void write_file( const char *path, char value )
{
    int fd = open( path, O_WRONLY | O_CREAT | O_EXCL, 0600 );

    assert( fd != -1 );
    assert( write( fd, &value, 1 ) == 1 );
    assert( !close( fd ));
}

static char read_child( const char *directory )
{
    char *path = wine_ps5_title_join_path( directory, "Child.TXT" ), byte;
    int fd;

    assert( path );
    fd = open( path, O_RDONLY );
    assert( fd != -1 );
    assert( read( fd, &byte, 1 ) == 1 );
    assert( !close( fd ));
    free( path );
    return byte;
}

static void check_join( const char *directory, const char *name, const char *expected )
{
    char *path = wine_ps5_title_join_path( directory, name );

    assert( path && !strcmp( path, expected ));
    free( path );
}

static void test_received_directory(void)
{
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    struct wine_ps5_title_directory state;
    char base[] = "/tmp/wine-ps5-title-XXXXXX";
    char original[512], moved[512], original_file[512], moved_file[512];
    const char *current = NULL;
    sigset_t blocked, previous, now;
    int sent, received, file;

    check_join( "/a", "b/c", "/a/b/c" );
    check_join( "/a/", "b", "/a/b" );
    check_join( "/", "b", "/b" );
    check_join( "/a", "/x", "/x" );

    assert( mkdtemp( base ));
    assert( snprintf( original, sizeof(original), "%s/original", base ) < sizeof(original) );
    assert( snprintf( moved, sizeof(moved), "%s/moved", base ) < sizeof(moved) );
    assert( snprintf( original_file, sizeof(original_file), "%s/Child.TXT", original ) < sizeof(original_file) );
    assert( snprintf( moved_file, sizeof(moved_file), "%s/Child.TXT", moved ) < sizeof(moved_file) );
    assert( !mkdir( original, 0700 ));
    write_file( original_file, 'a' );
    sent = open( original, O_RDONLY | O_DIRECTORY );
    assert( sent != -1 );
    received = transfer_directory( sent );  /* a new number, as from the private server */
    assert( !close( sent ));
    wine_ps5_title_critical_signals( &blocked );
    assert( !pthread_sigmask( SIG_BLOCK, NULL, &previous ));

    assert( !wine_ps5_title_enter_directory( &mutex, &blocked, &current, received, original, &state ));
    assert( current == original );
    assert( !pthread_sigmask( SIG_BLOCK, NULL, &now ));
    assert( sigismember( &now, SIGQUIT ) && sigismember( &now, SIGTERM ) && sigismember( &now, SIGUSR1 ));
    assert( read_child( current ) == 'a' );
    wine_ps5_title_leave_directory( &mutex, &current, &state );
    assert( !current );
    assert( !pthread_sigmask( SIG_BLOCK, NULL, &now ));
    assert( sigismember( &now, SIGQUIT ) == sigismember( &previous, SIGQUIT ));

    /* The held directory moves and a decoy takes its old name: its recorded
     * path must be refused, never resolved to the replacement's files. */
    assert( !rename( original, moved ));
    assert( !mkdir( original, 0700 ));
    write_file( original_file, 'b' );
    errno = 0;
    assert( wine_ps5_title_check_directory( received, original ) == -1 && errno == ENOENT );
    errno = 0;
    assert( wine_ps5_title_enter_directory( &mutex, &blocked, &current, received, original, &state ) == -1 );
    assert( errno == ENOENT && !current );
    assert( !pthread_mutex_trylock( &mutex ));  /* a failed enter rolls back the lock */
    pthread_mutex_unlock( &mutex );
    assert( !pthread_sigmask( SIG_BLOCK, NULL, &now ));
    assert( sigismember( &now, SIGQUIT ) == sigismember( &previous, SIGQUIT ));
    assert( !wine_ps5_title_check_directory( received, moved ));
    assert( read_child( moved ) == 'a' );

    errno = 0;
    assert( wine_ps5_title_check_directory( received, "moved" ) == -1 && errno == EINVAL );
    errno = 0;
    assert( wine_ps5_title_check_directory( -1, moved ) == -1 && errno == EBADF );
    file = open( moved_file, O_RDONLY );
    assert( file != -1 );
    errno = 0;
    assert( wine_ps5_title_check_directory( file, moved_file ) == -1 && errno == ENOTDIR );
    assert( !close( file ));

    assert( !close( received ));
    assert( !unlink( original_file ));
    assert( !unlink( moved_file ));
    assert( !rmdir( original ));
    assert( !rmdir( moved ));
    assert( !rmdir( base ));
    pthread_mutex_destroy( &mutex );
}

struct cache_exit_test
{
    pthread_mutex_t mutex;
    sigset_t blocked;
    int control[2];
};

static void *cache_request_client( void *arg )
{
    struct cache_exit_test *test = arg;
    struct wine_ps5_title_lock cache;
    char byte = 1;

    wine_ps5_title_lock_mutex( &cache, &test->mutex, &test->blocked );
    pthread_cleanup_push( wine_ps5_title_unlock_mutex, &cache );
    assert( write( test->control[1], &byte, 1 ) == 1 );
    assert( read( test->control[1], &byte, 1 ) == 0 );
    pthread_exit( NULL );  /* a disconnected server aborts the cache requester */
    pthread_cleanup_pop( 1 );
    return NULL;
}

static void test_native_exit_with_cache_lock(void)
{
    struct cache_exit_test test = { .mutex = PTHREAD_MUTEX_INITIALIZER };
    pthread_t client;
    char byte;

    wine_ps5_title_critical_signals( &test.blocked );
    assert( !socketpair( AF_UNIX, SOCK_STREAM, 0, test.control ));
    assert( !pthread_create( &client, NULL, cache_request_client, &test ));
    assert( read( test.control[0], &byte, 1 ) == 1 );
    assert( !shutdown( test.control[0], SHUT_RDWR ));
    assert( !pthread_join( client, NULL ));
    assert( !pthread_mutex_trylock( &test.mutex ));
    pthread_mutex_unlock( &test.mutex );
    close( test.control[0] );
    close( test.control[1] );
    pthread_mutex_destroy( &test.mutex );
}

struct quit_directory_test
{
    pthread_mutex_t mutex;
    sigset_t blocked;
    const char *current;
    char path[32];
    int directory, control[2];
};

static volatile sig_atomic_t quit_delivered;

static void directory_quit_handler( int sig )
{
    quit_delivered = 1;
    pthread_exit( NULL );  /* same nonlocal thread termination as Wine's handler */
}

static void *quit_directory_client( void *arg )
{
    struct quit_directory_test *test = arg;
    struct wine_ps5_title_directory state;
    sigset_t pending;
    char byte = 1;

    assert( !wine_ps5_title_enter_directory( &test->mutex, &test->blocked, &test->current,
                                             test->directory, test->path, &state ));
    assert( write( test->control[1], &byte, 1 ) == 1 );
    assert( read( test->control[1], &byte, 1 ) == 1 );
    assert( !sigpending( &pending ));
    assert( sigismember( &pending, SIGQUIT ));
    /* Pending SIGQUIT may terminate this thread during mask restoration,
     * but only after the directory was left and the mutex released. */
    wine_ps5_title_leave_directory( &test->mutex, &test->current, &state );
    assert( 0 && "pending SIGQUIT did not terminate the client" );
    return NULL;
}

static void test_quit_during_directory_operation(void)
{
    struct quit_directory_test test = { .mutex = PTHREAD_MUTEX_INITIALIZER,
                                        .path = "/tmp/wine-ps5-quit-XXXXXX" };
    struct sigaction action = {0}, previous_action;
    struct wine_ps5_title_directory state;
    sigset_t quit, previous;
    pthread_t client;
    char byte;

    assert( mkdtemp( test.path ));
    test.directory = open( test.path, O_RDONLY | O_DIRECTORY );
    assert( test.directory != -1 );
    assert( !socketpair( AF_UNIX, SOCK_STREAM, 0, test.control ));
    wine_ps5_title_critical_signals( &test.blocked );
    sigemptyset( &quit );
    sigaddset( &quit, SIGQUIT );
    assert( !pthread_sigmask( SIG_UNBLOCK, &quit, &previous ));
    action.sa_handler = directory_quit_handler;
    sigemptyset( &action.sa_mask );
    assert( !sigaction( SIGQUIT, &action, &previous_action ));
    quit_delivered = 0;
    assert( !pthread_create( &client, NULL, quit_directory_client, &test ));
    assert( read( test.control[0], &byte, 1 ) == 1 );
    assert( !pthread_kill( client, SIGQUIT ));
    assert( write( test.control[0], &byte, 1 ) == 1 );
    assert( !pthread_join( client, NULL ));
    assert( quit_delivered && !test.current );
    assert( !pthread_mutex_trylock( &test.mutex ));
    pthread_mutex_unlock( &test.mutex );
    /* A subsequent directory operation must still be usable. */
    assert( !wine_ps5_title_enter_directory( &test.mutex, &test.blocked, &test.current,
                                             test.directory, test.path, &state ));
    wine_ps5_title_leave_directory( &test.mutex, &test.current, &state );
    assert( !sigaction( SIGQUIT, &previous_action, NULL ));
    assert( !pthread_sigmask( SIG_SETMASK, &previous, NULL ));
    close( test.control[0] );
    close( test.control[1] );
    close( test.directory );
    assert( !rmdir( test.path ));
    pthread_mutex_destroy( &test.mutex );
}

static int counted_regular_joins;

static int counted_regular_join( pthread_t thread, void **result )
{
    counted_regular_joins++;
    return pthread_join( thread, result );
}

static void *recycled_worker( void *arg )
{
    int *control = arg;
    char byte;

    assert( read( control[1], &byte, 1 ) == 0 );
    return (void *)1;
}

static void test_lifetime_tickets(void)
{
    struct wine_ps5_title_lifetimes lifetimes = WINE_PS5_TITLE_LIFETIMES_INIT;
    struct wine_ps5_title_join initial = WINE_PS5_TITLE_JOIN_INIT;
    struct wine_ps5_title_threads registry = WINE_PS5_TITLE_THREADS_INIT;
    struct wine_ps5_title_thread first = { 0 }, recycled = { 0 };
    static const char teb;
    pthread_t worker;
    int control[2];

    assert( wine_ps5_title_new_lifetime( &lifetimes ) == 1 );
    assert( wine_ps5_title_new_lifetime( &lifetimes ) == 2 );
    lifetimes.next = UINT64_MAX;
    assert( wine_ps5_title_new_lifetime( &lifetimes ) == UINT64_MAX );
    /* Exhaustion is permanent: ticket 1 must never be handed out again. */
    assert( wine_ps5_title_new_lifetime( &lifetimes ) == 0 );
    assert( wine_ps5_title_new_lifetime( &lifetimes ) == 0 );

    /* The initial thread exited and was reaped early. */
    initial.lifetime = first.lifetime = 7;
    assert( !pthread_create( &initial.thread, NULL, initial_windows_thread, NULL ));
    initial_join_test = &(struct initial_join_test){ .mutex = PTHREAD_MUTEX_INITIALIZER,
                                                      .condition = PTHREAD_COND_INITIALIZER };
    assert( !wine_ps5_title_join_client( &first, initial.thread, &initial,
                                         counted_initial_join, counted_regular_join ));
    assert( initial_join_test->calls == 1 && !counted_regular_joins );

    /* A later worker reuses the Windows ID and TEB but has its own lifetime;
     * shutdown must not signal it and its reaper must really join it. */
    assert( !socketpair( AF_UNIX, SOCK_STREAM, 0, control ));
    assert( !pthread_create( &worker, NULL, recycled_worker, control ));
    recycled.lifetime = 8;
    assert( !wine_ps5_title_register_thread( &registry, &recycled, 4, &teb, worker ));
    errno = 0;
    assert( wine_ps5_title_signal_lifetime( &registry, initial.lifetime, 0 ) == -1 && errno == ESRCH );
    assert( !wine_ps5_title_signal_lifetime( &registry, recycled.lifetime, 0 ));
    assert( wine_ps5_title_signal_lifetime( &registry, 0, 0 ) == -1 && errno == ESRCH );
    wine_ps5_title_unregister_thread( &registry, &recycled );
    assert( !shutdown( control[0], SHUT_RDWR ));
    assert( !wine_ps5_title_join_client( &recycled, worker, &initial,
                                         counted_initial_join, counted_regular_join ));
    assert( initial_join_test->calls == 1 && counted_regular_joins == 1 );
    close( control[0] );
    close( control[1] );

    /* Outside title mode every lifetime is 0 and joins stay ordinary. */
    recycled.lifetime = initial.lifetime = 0;
    assert( !pthread_create( &worker, NULL, initial_windows_thread, NULL ));
    assert( !wine_ps5_title_join_client( &recycled, worker, &initial,
                                         counted_initial_join, counted_regular_join ));
    assert( initial_join_test->calls == 1 && counted_regular_joins == 2 );
    initial_join_test = NULL;
}

static void *startup_reply_reader( void *arg )
{
    int *reply = arg;
    char byte;

    /* Like read_reply_data: this thread still owns the reply writer. */
    assert( read( reply[0], &byte, 1 ) == 0 );
    return NULL;
}

static void test_startup_cancellation(void)
{
    struct wine_ps5_title_startup startup = WINE_PS5_TITLE_STARTUP_INIT;
    struct wine_ps5_title_startup done = WINE_PS5_TITLE_STARTUP_INIT;
    pthread_t client;
    int reply[2], late[2], live[2], i;
    char byte = 1;

    assert( !socketpair( AF_UNIX, SOCK_STREAM, 0, reply ));
    assert( !wine_ps5_title_publish_startup_channel( &startup, WINE_PS5_TITLE_STARTUP_REPLY_READ, reply[0] ));
    assert( !wine_ps5_title_publish_startup_channel( &startup, WINE_PS5_TITLE_STARTUP_REPLY_WRITE, reply[1] ));
    assert( !pthread_create( &client, NULL, startup_reply_reader, reply ));
    wine_ps5_title_cancel_startup( &startup );
    assert( !pthread_join( client, NULL ));
    for (i = 0; i < WINE_PS5_TITLE_STARTUP_CHANNEL_COUNT; i++) assert( startup.channel[i] == -1 );

    /* A channel published after cancellation is shut down immediately. */
    assert( !socketpair( AF_UNIX, SOCK_STREAM, 0, late ));
    errno = 0;
    assert( wine_ps5_title_publish_startup_channel( &startup, WINE_PS5_TITLE_STARTUP_WAIT_READ, late[0] ) == -1 );
    assert( errno == ECANCELED );
    assert( read( late[0], &byte, 1 ) == 0 );
    assert( startup.channel[WINE_PS5_TITLE_STARTUP_WAIT_READ] == -1 );
    errno = 0;
    assert( wine_ps5_title_complete_startup( &startup ) == -1 && errno == ECANCELED );

    /* After completion, cancellation must leave the client's sockets alone. */
    assert( !socketpair( AF_UNIX, SOCK_STREAM, 0, live ));
    assert( !wine_ps5_title_publish_startup_channel( &done, WINE_PS5_TITLE_STARTUP_REQUEST, live[0] ));
    assert( !wine_ps5_title_complete_startup( &done ));
    assert( done.channel[WINE_PS5_TITLE_STARTUP_REQUEST] == -1 );
    assert( !wine_ps5_title_publish_startup_channel( &done, WINE_PS5_TITLE_STARTUP_MESSAGE, live[0] ));
    assert( done.channel[WINE_PS5_TITLE_STARTUP_MESSAGE] == -1 );
    wine_ps5_title_cancel_startup( &done );
    assert( write( live[1], &byte, 1 ) == 1 );
    assert( read( live[0], &byte, 1 ) == 1 );
    for (i = 0; i < 2; i++)
    {
        close( reply[i] );
        close( late[i] );
        close( live[i] );
    }
    pthread_mutex_destroy( &startup.mutex );
    pthread_mutex_destroy( &done.mutex );
}

static int copy_calls, copy_fail_at;

static int failing_copy( void *dest, const void *src, size_t size )
{
    if (++copy_calls == copy_fail_at) return -1;
    memcpy( dest, src, size );
    return 0;
}

static void test_directory_output(void)
{
    struct wine_ps5_title_directory_output output = { 0 };
    unsigned char buffer[64], entry[20];
    uint32_t next;
    size_t information = 0;

    memset( buffer, 0xcc, sizeof(buffer) );
    memset( entry, 0, sizeof(entry) );
    assert( !wine_ps5_title_append_directory_output( &output, buffer, &information, entry, 13, 8, failing_copy ));
    assert( output.used == 13 && output.previous == 0 && output.have_entry && information == 13 );
    assert( !wine_ps5_title_append_directory_output( &output, buffer, &information, entry, 20, 8, failing_copy ));
    memcpy( &next, buffer, sizeof(next) );
    assert( next == 16 && output.used == 36 && output.previous == 16 && information == 36 );

    /* A caller fault while linking the previous entry leaves the cursor at
     * the last fully published entry, so that entry is not skipped. */
    copy_calls = 0;
    copy_fail_at = 3;
    assert( wine_ps5_title_append_directory_output( &output, buffer, &information, entry, 8, 8, failing_copy ) == -1 );
    memcpy( &next, buffer + 16, sizeof(next) );
    assert( next == 0 && output.used == 36 && output.previous == 16 );
    copy_fail_at = 0;
}

struct fault_test
{
    pthread_mutex_t mutex;
    sigset_t blocked;
    void *page;
};

static void fault_handler( int sig, siginfo_t *info, void *context )
{
    void *jmp = fault_jmp;

    if (!jmp) abort();
    fault_jmp = NULL;
    longjmp( jmp, 1 );
}

static void *faulting_directory_client( void *arg )
{
    struct fault_test *test = arg;
    struct wine_ps5_title_lock cache;
    sigset_t before, current;
    uint64_t value = 1, copy = 0;

    pthread_sigmask( SIG_SETMASK, NULL, &before );
    wine_ps5_title_lock_mutex( &cache, &test->mutex, &test->blocked );
    pthread_cleanup_push( wine_ps5_title_unlock_mutex, &cache );
    assert( wine_ps5_title_fault_safe_copy( test->page, &value, sizeof(value) ) == -1 );
    assert( !fault_jmp );
    pthread_sigmask( SIG_SETMASK, NULL, &current );
    assert( sigismember( &current, SIGQUIT ));  /* still inside the cache lock */
    assert( !wine_ps5_title_fault_safe_copy( &copy, &value, sizeof(value) ) && copy == 1 );
    assert( !fault_jmp );
    pthread_cleanup_pop( 1 );
    pthread_sigmask( SIG_SETMASK, NULL, &current );
    assert( sigismember( &current, SIGQUIT ) == sigismember( &before, SIGQUIT ));
    /* No native cleanup frame survives the fault, so exit runs none. */
    pthread_exit( NULL );
    return NULL;
}

static void test_directory_fault(void)
{
    struct fault_test test = { .mutex = PTHREAD_MUTEX_INITIALIZER };
    struct sigaction action = { .sa_sigaction = fault_handler, .sa_flags = SA_SIGINFO }, old_segv, old_bus;
    pthread_t client;
    int i;

    wine_ps5_title_critical_signals( &test.blocked );
    test.page = mmap( NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0 );
    assert( test.page != MAP_FAILED );
    sigemptyset( &action.sa_mask );
    assert( !sigaction( SIGSEGV, &action, &old_segv ));
    assert( !sigaction( SIGBUS, &action, &old_bus ));
    /* A second query after the faulting client exits must find the lock free. */
    for (i = 0; i < 2; i++)
    {
        assert( !pthread_create( &client, NULL, faulting_directory_client, &test ));
        assert( !pthread_join( client, NULL ));
    }
    assert( !pthread_mutex_trylock( &test.mutex ));
    pthread_mutex_unlock( &test.mutex );
    assert( !sigaction( SIGSEGV, &old_segv, NULL ));
    assert( !sigaction( SIGBUS, &old_bus, NULL ));
    munmap( test.page, 4096 );
    pthread_mutex_destroy( &test.mutex );
}

static void test_radv_windows(void)
{
    uint64_t low, high;

    assert( !wine_ps5_title_radv_overlap( 0, 0, NULL, NULL ));
    assert( !wine_ps5_title_radv_overlap( UINT64_C(0x1ffff0000), 0x10000, NULL, NULL ));
    assert( wine_ps5_title_radv_overlap( UINT64_C(0x1ffff0000), 0x10001, &low, &high ));
    assert( low == UINT64_C(0x200000000) && high == UINT64_C(0x300000000) );
    assert( wine_ps5_title_radv_overlap( UINT64_C(0x200000000), 1, NULL, NULL ));
    assert( wine_ps5_title_radv_overlap( UINT64_C(0x2ffffffff), 1, NULL, NULL ));
    assert( !wine_ps5_title_radv_overlap( UINT64_C(0x300000000), 0x10000, NULL, NULL ));
    assert( !wine_ps5_title_radv_overlap( UINT64_C(0x1000000000), UINT64_C(0x100000000), NULL, NULL ));
    assert( !wine_ps5_title_radv_overlap( UINT64_C(0x3fffff0000), 0x10000, NULL, NULL ));
    assert( wine_ps5_title_radv_overlap( UINT64_C(0x3fffff0000), 0x10001, &low, &high ));
    assert( low == UINT64_C(0x4000000000) && high == UINT64_C(0x8000000000) );
    assert( wine_ps5_title_radv_overlap( UINT64_C(0x4000000000), 1, NULL, NULL ));
    assert( wine_ps5_title_radv_overlap( UINT64_C(0x7fffffffff), 1, NULL, NULL ));
    assert( !wine_ps5_title_radv_overlap( UINT64_C(0x8000000000), 0x10000, NULL, NULL ));
    assert( !wine_ps5_title_radv_overlap( UINT64_MAX, UINT64_MAX, NULL, NULL ));
}

static void test_direct_cache_image_gate(void)
{
    static const struct { const char *image; int allowed; } cases[] = {
        { "C:\\ProgramData\\Battle.net\\Agent\\Agent.9824\\Agent.exe", 1 },
        { "C:\\Agent.exe", 1 }, /* The independent scan probe uses this image spelling. */
        { "aGeNt.ExE", 1 },
        { "/prefix/drive_c/Agent.exe", 1 },
        { "C:\\Program Files (x86)\\Battle.net\\Battle.net.exe", 0 },
        { "C:\\ProgramData\\Battle.net\\Agent\\AgentHelper.exe", 0 },
        { "C:\\Agent.exe\\Battle.net.exe", 0 },
        { "C:\\NotAgent.exe", 0 },
        { "C:\\Agent.exe.old", 0 },
        { "Agent.exe --type=renderer", 0 },
        { "\"C:\\Agent.exe\"", 0 },
        { "C:Agent.exe", 0 },
        { "", 0 }
    };
    uint16_t image[256];
    size_t i, j, length;

    assert( !wine_ps5_title_direct_cache_image( NULL, 0 ));
    assert( !wine_ps5_title_direct_cache_image( NULL, 9 ));
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        length = strlen( cases[i].image );
        assert( length < sizeof(image) / sizeof(image[0]) );
        for (j = 0; j < length; j++) image[j] = (unsigned char)cases[i].image[j];
        image[length] = 0xa55a; /* UNICODE_STRING length, not an argv/NUL-string scan. */
        assert( wine_ps5_title_direct_cache_image( image, length ) == cases[i].allowed );
    }
    puts( "ps5 direct cache image: exact Agent.exe leaf=1 cef_excluded=1 helper_excluded=1 bounded_utf16=1 PASS" );
}

static void test_title_driver_load(void)
{
    assert( wine_ps5_title_driver_loaded( 0, 1 ));
    assert( !wine_ps5_title_driver_loaded( 0, 0 )); /* DLL loaded, ABI rejected */
    assert( !wine_ps5_title_driver_loaded( 0xc0000135, 0 )); /* DLL missing */
    assert( !wine_ps5_title_driver_loaded( 0xc0000142, 1 )); /* attach failed */
}

static void test_title_stdout_binding(void)
{
    static const char expected[] = "psmkit-wine PE: cpu-only works=1\r\n";
    struct wine_ps5_title_runtime runtime = {
        .stdout_fd = 1, .version = WINE_PS5_TITLE_RUNTIME_VERSION
    }, startup_copy;
    struct stat before, after;
    int pipefd[2], original_flags;
    char received[sizeof(expected)];

    assert( wine_ps5_title_stdout_fd( NULL ) == 1 ); /* payload/no native runtime */
    assert( wine_ps5_title_stdout_fd( &runtime ) == 1 ); /* existing native title */
    original_flags = fcntl( 1, F_GETFD );
    assert( original_flags >= 0 && !fstat( 1, &before ));
    assert( !pipe( pipefd ));
    runtime.stdout_fd = pipefd[1];
    assert( wine_ps5_title_stdout_valid( &runtime ));
    startup_copy = runtime; /* loader->initial ntdll thread preserves both ABI fields */
    assert( startup_copy.version == WINE_PS5_TITLE_RUNTIME_VERSION );
    assert( wine_ps5_title_stdout_fd( &startup_copy ) == pipefd[1] );
    assert( !isatty( wine_ps5_title_stdout_fd( &startup_copy )));
    assert( write( wine_ps5_title_stdout_fd( &startup_copy ), expected, sizeof(expected) - 1 )
            == sizeof(expected) - 1 );
    assert( read( pipefd[0], received, sizeof(received) ) == sizeof(expected) - 1 );
    assert( !memcmp( received, expected, sizeof(expected) - 1 ));
    assert( !fstat( 1, &after ) && fcntl( 1, F_GETFD ) == original_flags );
    assert( before.st_dev == after.st_dev && before.st_ino == after.st_ino && before.st_mode == after.st_mode );
    runtime.version++;
    assert( !wine_ps5_title_stdout_valid( &runtime ));
    runtime.version = WINE_PS5_TITLE_RUNTIME_VERSION;
    runtime.stdout_fd = pipefd[0]; /* read-only endpoint cannot become Windows stdout */
    assert( !wine_ps5_title_stdout_valid( &runtime ));
    runtime.stdout_fd = -1;
    assert( !wine_ps5_title_stdout_valid( &runtime ));
    runtime.stdout_fd = pipefd[1];
    assert( !close( pipefd[1] ));
    assert( !wine_ps5_title_stdout_valid( &runtime )); /* closed handle is rejected */
    assert( !close( pipefd[0] ));
    puts( "ps5 stdout binding: real pipe bytes, version/FD validation, native fd1 unchanged; Wine/PE NOT RUN" );
}

static int copy_hdr_luminance( struct wine_ps5_hdr_luminance *luminance )
{
    *luminance = (struct wine_ps5_hdr_luminance)
    {
        .max_full_frame_luminance = 637.3189697265625f,
        .max_luminance = 981.14599609375f,
        .min_luminance = 0.12493500113487244f,
    };
    return 0;
}

static float float_distance( float a, float b )
{
    return a > b ? a - b : b - a;
}

static void test_generic_identity(void)
{
    static const struct
    {
        const char *field, *value;
    } fields[] =
    {
        { "sys_vendor", "The Wine project" },
        { "product_name", "Wine" },
        { "product_version", "test-package-version" },
        { "bios_vendor", "The Wine project" },
        { "bios_version", "test-package-version" },
        { "bios_date", "01/01/2021" },
        { "product_serial", "0" },
        { "product_sku", "" },
        { "product_family", "" },
        { "chassis_type", "" },
        { "chassis_vendor", "The Wine project" },
        { "chassis_version", "test-package-version" },
        { "chassis_serial", "0" },
        { "chassis_tag", "" },
        { "board_vendor", "The Wine project" },
        { "board_name", "Wine" },
        { "board_version", "test-package-version" },
        { "board_serial", "0" },
        { "board_asset_tag", "" },
        { "unknown", "" },
    };
    unsigned int i;

    for (i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
        assert( !strcmp( wine_ps5_smbios_string( fields[i].field, "test-package-version" ),
                         fields[i].value ));
    assert( !strcmp( wine_ps5_gpu_name(), "AMD Radeon Graphics (RADV)" ));
    assert( WINE_PS5_GPU_VENDOR_ID == 0x1002 );
    assert( WINE_PS5_GPU_DEVICE_ID == 0x13fb );
    puts( "ps5 identity: generic Wine SMBIOS, unavailable DMI fields, RADV name and real PCI IDs PASS" );
}


static void test_sdr_white_level_conversion(void)
{
    assert( !wine_ps5_sdr_white_level_from_nits( WINE_PS5_SDR_WHITE_MIN_NITS - 1 ));
    assert( wine_ps5_sdr_white_level_from_nits( WINE_PS5_SDR_WHITE_MIN_NITS ) == 1000 );
    assert( wine_ps5_sdr_white_level_from_nits( 81 ) == 1013 );
    assert( wine_ps5_sdr_white_level_from_nits( WINE_PS5_SDR_WHITE_DEFAULT_NITS ) == 2538 );
    assert( wine_ps5_sdr_white_level_from_nits( WINE_PS5_SDR_WHITE_MAX_NITS ) == 6000 );
    assert( !wine_ps5_sdr_white_level_from_nits( WINE_PS5_SDR_WHITE_MAX_NITS + 1 ));
    assert( !wine_ps5_sdr_white_level_from_nits( ~0u ));
    puts( "ps5 SDR white level: 80..480-nit bounds and rounded Windows conversion PASS" );
}

static void test_hdr_luminance_edid(void)
{
    struct wine_ps5_title_runtime runtime =
    {
        .get_hdr_luminance = copy_hdr_luminance,
        .stdout_fd = 1,
        .server_socket = -1,
        .version = WINE_PS5_TITLE_RUNTIME_VERSION,
    }, installed = runtime;
    struct wine_ps5_hdr_luminance luminance, invalid;
    unsigned char edid[WINE_PS5_HDR_EDID_SIZE];
    unsigned int block, i, sum;
    float encoded_max;

    _Static_assert( sizeof(luminance) == 12, "title HDR luminance ABI" );
    assert( installed.version == WINE_PS5_TITLE_RUNTIME_VERSION );
    assert( !installed.get_hdr_luminance( &luminance ));
    assert( wine_ps5_build_hdr_edid( edid, &luminance, 3840, 2160 ) == sizeof(edid) );

    for (block = 0; block < 2; block++)
    {
        for (sum = i = 0; i < 128; i++) sum += edid[block * 128 + i];
        assert( !(sum & 0xff) );
    }
    assert( !memcmp( edid, "\x00\xff\xff\xff\xff\xff\xff\x00", 8 ) );
    assert( edid[8] == 0x5d && edid[9] == 0x2e ); /* EISA WIN. */
    assert( edid[75] == 0xfc && !memcmp( edid + 77, "Wine HDR TV\n ", 13 ));
    assert( edid[126] == 1 && edid[128] == 0x02 && edid[129] == 0x03 );
    assert( (edid[56] | ((edid[58] & 0xf0) << 4)) == 3840 );
    assert( (edid[59] | ((edid[61] & 0xf0) << 4)) == 2160 );
    assert( edid[132] == 0xe3 && edid[133] == 0x05 && edid[134] == 0x80 );
    assert( edid[136] == 0xe6 && edid[137] == 0x06 );
    assert( edid[138] == 0x05 && edid[139] == 0x01 ); /* SDR + PQ, static metadata type 1. */
    assert( edid[140] == 137 ); /* 50 * 2^(137 / 32) = 972.198 cd/m2. */
    assert( edid[141] == 118 ); /* 50 * 2^(118 / 32) = 644.196 cd/m2. */
    assert( edid[142] == 29 );
    encoded_max = wine_ps5_hdr_decode_max_luminance( edid[140] );
    assert( float_distance( encoded_max, 972.1979f ) < 0.001f );
    assert( float_distance( wine_ps5_hdr_decode_min_luminance( encoded_max, edid[142] ),
                            0.1257391f ) < 0.000001f );

    /* Base-block BT.2020/D65 chromaticities are 10-bit fractions. */
    assert( ((edid[27] << 2) | (edid[25] >> 6)) == 725 );
    assert( ((edid[28] << 2) | ((edid[25] >> 4) & 3)) == 299 );
    assert( ((edid[29] << 2) | ((edid[25] >> 2) & 3)) == 174 );
    assert( ((edid[30] << 2) | (edid[25] & 3)) == 816 );
    assert( ((edid[31] << 2) | (edid[26] >> 6)) == 134 );
    assert( ((edid[32] << 2) | ((edid[26] >> 4) & 3)) == 47 );
    assert( ((edid[33] << 2) | ((edid[26] >> 2) & 3)) == 320 );
    assert( ((edid[34] << 2) | (edid[26] & 3)) == 337 );

    invalid = luminance;
    invalid.max_full_frame_luminance = invalid.max_luminance + 1.0f;
    assert( !wine_ps5_build_hdr_edid( edid, &invalid, 3840, 2160 ) );
    puts( "ps5 hdr edid: Wine identity, runtime ABI, CTA luminance codes, BT.2020/D65 and checksums PASS" );
}

static int game_loader_exists( const char *path )
{
    return !strcmp( path, "/prefix/drive_c/WoW/WowB_loader.dll" ) ||
           !strcmp( path, "/prefix/drive_c/WoW/wOwTest_loader.dll" ) ||
           !strcmp( path, "/prefix/drive_c/WoW/wow_loader.dll" );
}

static void test_game_image_rule(void)
{
    assert( wine_ps5_title_game_image( "C:\\WoW\\WowB.exe",
                                      "/prefix/drive_c/WoW/WowB.exe", game_loader_exists ) );
    assert( wine_ps5_title_game_image( "w:\\WoW\\wOwTest.ExE",
                                      "/prefix/drive_c/WoW/wOwTest.ExE", game_loader_exists ) );
    assert( wine_ps5_title_game_image( "C:\\WoW\\WoW.exe",
                                      "/prefix/drive_c/WoW/WoW.exe", game_loader_exists ) );
    assert( !wine_ps5_title_game_image( "C:\\WoW\\WowVoiceProxy.exe",
                                       "/prefix/drive_c/WoW/WowVoiceProxy.exe", game_loader_exists ) );
    assert( !wine_ps5_title_game_image( "Z:\\WoW\\WowB.exe",
                                       "/prefix/drive_c/WoW/WowB.exe", game_loader_exists ) );
    assert( !wine_ps5_title_game_image( "C:\\WoW\\Other.exe",
                                       "/prefix/drive_c/WoW/Other.exe", game_loader_exists ) );
    puts( "ps5 game image: Wow*.exe plus sibling loader, C/W drives, case-insensitive rule PASS" );
}

static const char *match_refused_program( const char *image, const char *programs, size_t *match_length )
{
    uint16_t wide[512];
    size_t length = strlen( image ), i;

    assert( length < sizeof(wide) / sizeof(wide[0]) );
    for (i = 0; i < length; i++) wide[i] = (unsigned char)image[i];
    return wine_ps5_title_refused_program( wide, length, programs, match_length );
}

static void test_refused_program_rule(void)
{
    static const char programs[] = "OtherReporter.exe;BlizzardError.exe";
    const char *match;
    size_t length;

    match = match_refused_program( "\\??\\C:\\Games\\Errors\\bLiZzArDeRrOr.ExE", programs, &length );
    assert( match == programs + strlen("OtherReporter.exe;") );
    assert( length == strlen("BlizzardError.exe") );
    assert( match_refused_program( "D:/Helpers/BLIZZARDERROR.EXE", programs, &length ) == match );
    assert( match_refused_program( "BlizzardError.exe", programs, &length ) == match );
    assert( !match_refused_program( "C:\\Errors\\NotBlizzardError.exe", programs, &length ) );
    assert( !match_refused_program( "C:\\Errors\\BlizzardError.exe.old", programs, &length ) );
    assert( !match_refused_program( "C:\\Errors\\BlizzardError.exe", "", &length ) );
    assert( !match_refused_program( "C:\\Errors\\BlizzardError.exe",
                                    "C:\\Errors\\BlizzardError.exe", &length ) );
    puts( "ps5 refused program: basename-only profile list and path/case-insensitive match PASS" );
}

static void test_write_routing(void)
{
    static const uint64_t run120_stalls[] =
    {
        UINT64_C(500000), UINT64_C(575000), UINT64_C(650000)
    };
    struct wine_ps5_title_write_state state = WINE_PS5_TITLE_WRITE_STATE_INIT;
    struct wine_ps5_title_write_state spaced = WINE_PS5_TITLE_WRITE_STATE_INIT;
    struct wine_ps5_title_write_state sliding = WINE_PS5_TITLE_WRITE_STATE_INIT;
    struct wine_ps5_title_write_state run120 = WINE_PS5_TITLE_WRITE_STATE_INIT;
    struct wine_ps5_title_write_state volume = WINE_PS5_TITLE_WRITE_STATE_INIT;
    struct wine_ps5_title_write_decision decision;
    uint64_t now = UINT64_C(1000000), deadline, total = 0;
    uint64_t run120_total = 0, burst_start = now + UINT64_C(20000000);
    uint64_t mask_at, throttle_start;
    unsigned int i;

    assert( WINE_PS5_WRITE_THROTTLE_US == UINT64_C(700000) );
    decision = wine_ps5_title_write_route( state, 0, WINE_PS5_WRITE_LOCAL_NONE,
                                           0, 0, now, 1, 0 );
    assert( decision.route == WINE_PS5_WRITE_LOCAL );
    decision = wine_ps5_title_write_route( state, WINE_PS5_WRITE_SMALL_MAX,
                                           WINE_PS5_WRITE_LOCAL_NONE, 0, 0, now, 1, 0 );
    assert( decision.route == WINE_PS5_WRITE_LOCAL );
    decision = wine_ps5_title_write_route( state, WINE_PS5_WRITE_SMALL_MAX + 1,
                                           WINE_PS5_WRITE_LOCAL_NONE, 0, 0, now, 1, 0 );
    assert( decision.route == WINE_PS5_WRITE_BROKER );
    decision = wine_ps5_title_write_route( state, WINE_PS5_WRITE_SMALL_MAX + 1,
                                           WINE_PS5_WRITE_LOCAL_NONE, 0, 0, now, 0, 0 );
    assert( decision.route == WINE_PS5_WRITE_LOCAL );

    /* Run 40/105-run109/114: fast writes must not reset real 0.76-0.95-s evidence. */
    decision = wine_ps5_title_write_route( state, 320, WINE_PS5_WRITE_THROTTLE_US,
                                           320, 320, now, 1, 0 );
    assert( decision.route == WINE_PS5_WRITE_LOCAL );
    assert( decision.state.mode == WINE_PS5_WRITE_NORMAL && decision.state.slow_count == 1 );
    state = decision.state;
    decision = wine_ps5_title_write_route( state, 320, WINE_PS5_WRITE_THROTTLE_US - 1,
                                           320, 640, now + UINT64_C(1000000), 1, 0 );
    assert( decision.route == WINE_PS5_WRITE_LOCAL && decision.state.slow_count == 1 );
    state = decision.state;
    decision = wine_ps5_title_write_route( state, 320, WINE_PS5_WRITE_THROTTLE_US,
                                           320, 960, now + UINT64_C(2000000), 1, 0 );
    assert( decision.route == WINE_PS5_WRITE_LOCAL && decision.state.slow_count == 2 );
    state = decision.state;
    decision = wine_ps5_title_write_route( state, 320, WINE_PS5_WRITE_THROTTLE_US - 1,
                                           320, 1280, now + UINT64_C(3000000), 1, 0 );
    assert( decision.route == WINE_PS5_WRITE_LOCAL && decision.state.slow_count == 2 );
    state = decision.state;
    decision = wine_ps5_title_write_route( state, 320, WINE_PS5_WRITE_THROTTLE_US,
                                           320, 1600, now + UINT64_C(4000000), 1, 0 );
    assert( decision.route == WINE_PS5_WRITE_BROKER );
    assert( decision.state.mode == WINE_PS5_WRITE_THROTTLED );
    assert( decision.transition == WINE_PS5_WRITE_SMALL_TO_BROKER );
    deadline = now + UINT64_C(4000000) + WINE_PS5_WRITE_BROKER_HOLD_US;
    assert( decision.state.broker_until_us == deadline );
    state = decision.state;

    decision = wine_ps5_title_write_route( state, 320, WINE_PS5_WRITE_LOCAL_NONE,
                                           0, 1600, deadline - 1, 1, 0 );
    assert( decision.route == WINE_PS5_WRITE_BROKER && !decision.probe );
    decision = wine_ps5_title_write_route( state, 320, WINE_PS5_WRITE_LOCAL_NONE,
                                           0, 1600, deadline - 1, 0, 0 );
    assert( decision.route == WINE_PS5_WRITE_LOCAL );
    decision = wine_ps5_title_write_route( state, WINE_PS5_WRITE_SMALL_MAX + 1,
                                           WINE_PS5_WRITE_LOCAL_NONE, 0, 1600, deadline, 1, 0 );
    assert( decision.route == WINE_PS5_WRITE_BROKER && !decision.probe );
    decision = wine_ps5_title_write_route( state, 320, WINE_PS5_WRITE_LOCAL_NONE,
                                           0, 1600, deadline, 1, 0 );
    assert( decision.route == WINE_PS5_WRITE_LOCAL && decision.probe );
    assert( decision.state.mode == WINE_PS5_WRITE_PROBING );
    state = decision.state;
    decision = wine_ps5_title_write_route( state, 320, WINE_PS5_WRITE_THROTTLE_US - 1,
                                           320, 1920, deadline + 1, 1, 0 );
    assert( decision.route == WINE_PS5_WRITE_BROKER );
    assert( decision.state.mode == WINE_PS5_WRITE_PROBING );
    decision = wine_ps5_title_write_route( state, 320, WINE_PS5_WRITE_THROTTLE_US - 1,
                                           320, 1920, deadline + 2, 1, 1 );
    assert( decision.route == WINE_PS5_WRITE_LOCAL );
    assert( decision.state.mode == WINE_PS5_WRITE_NORMAL );
    assert( decision.transition == WINE_PS5_WRITE_SMALL_TO_LOCAL );

    /* One isolated slow write, or two separated by more than five seconds, stays local. */
    decision = wine_ps5_title_write_route( spaced, 320, WINE_PS5_WRITE_THROTTLE_US,
                                           320, 320, now, 1, 0 );
    assert( decision.state.slow_count == 1 && decision.route == WINE_PS5_WRITE_LOCAL );
    spaced = decision.state;
    decision = wine_ps5_title_write_route( spaced, 320, WINE_PS5_WRITE_THROTTLE_US,
                                           320, 640,
                                           now + WINE_PS5_WRITE_SLOW_WINDOW_US + 1, 1, 0 );
    assert( decision.state.slow_count == 1 && decision.route == WINE_PS5_WRITE_LOCAL );
    assert( decision.state.slow_first_us == now + WINE_PS5_WRITE_SLOW_WINDOW_US + 1 );

    /* Sliding, not tumbling: t=4,6,8 seconds forms a window after t=0 expires. */
    decision = wine_ps5_title_write_route( sliding, 320, WINE_PS5_WRITE_THROTTLE_US,
                                           320, 320, now, 1, 0 );
    sliding = decision.state;
    decision = wine_ps5_title_write_route( sliding, 320, WINE_PS5_WRITE_THROTTLE_US,
                                           320, 640, now + UINT64_C(4000000), 1, 0 );
    sliding = decision.state;
    decision = wine_ps5_title_write_route( sliding, 320, WINE_PS5_WRITE_THROTTLE_US,
                                           320, 960, now + UINT64_C(6000000), 1, 0 );
    assert( decision.state.mode == WINE_PS5_WRITE_NORMAL && decision.state.slow_count == 2 );
    assert( decision.state.slow_first_us == now + UINT64_C(4000000) );
    sliding = decision.state;
    decision = wine_ps5_title_write_route( sliding, 320, WINE_PS5_WRITE_THROTTLE_US,
                                           320, 1280, now + UINT64_C(8000000), 1, 0 );
    assert( decision.state.mode == WINE_PS5_WRITE_THROTTLED );

    /* Run 120: repeated 64-KiB writes reach the 1-MiB contention threshold
     * without any one large write; observed 0.5-0.65-s stalls remain local. */
    for (i = 0; i < 32; i++)
    {
        run120_total = wine_ps5_title_write_local_total( run120_total,
                                                         WINE_PS5_WRITE_SMALL_MAX );
        decision = wine_ps5_title_write_route(
            run120, WINE_PS5_WRITE_SMALL_MAX, UINT64_C(1000),
            WINE_PS5_WRITE_SMALL_MAX, run120_total,
            burst_start + i * UINT64_C(50000), 1, 0 );
        assert( decision.route == WINE_PS5_WRITE_LOCAL );
        assert( decision.state.mode == WINE_PS5_WRITE_NORMAL &&
                !decision.state.slow_count );
        run120 = decision.state;
    }
    mask_at = burst_start + 31 * UINT64_C(50000);
    assert( run120.contention_us == mask_at && !run120.local_window_bytes );

    for (i = 0; i < 3; i++)
    {
        run120_total = wine_ps5_title_write_local_total( run120_total, 2 );
        decision = wine_ps5_title_write_route(
            run120, 2, run120_stalls[i], 2, run120_total,
            mask_at + (i + 1) * UINT64_C(600000), 1, 0 );
        assert( decision.route == WINE_PS5_WRITE_LOCAL );
        assert( decision.state.mode == WINE_PS5_WRITE_NORMAL &&
                !decision.state.slow_count );
        run120 = decision.state;
    }

    /* Even >=700-ms evidence is ignored while the cumulative-traffic mask is active. */
    run120_total = wine_ps5_title_write_local_total( run120_total, 2 );
    decision = wine_ps5_title_write_route(
        run120, 2, UINT64_C(900000), 2, run120_total,
        mask_at + WINE_PS5_WRITE_CONTENTION_US, 1, 0 );
    assert( decision.route == WINE_PS5_WRITE_LOCAL && !decision.state.slow_count );
    run120 = decision.state;

    /* Sustained 0.9-s small writes after the mask expires still select the broker. */
    throttle_start = mask_at + WINE_PS5_WRITE_CONTENTION_US + UINT64_C(900000);
    for (i = 0; i < WINE_PS5_WRITE_SLOW_COUNT; i++)
    {
        run120_total = wine_ps5_title_write_local_total( run120_total, 2 );
        decision = wine_ps5_title_write_route(
            run120, 2, UINT64_C(900000), 2, run120_total,
            throttle_start + i * UINT64_C(1000000), 1, 0 );
        run120 = decision.state;
        if (i + 1 < WINE_PS5_WRITE_SLOW_COUNT)
        {
            assert( decision.route == WINE_PS5_WRITE_LOCAL );
            assert( run120.mode == WINE_PS5_WRITE_NORMAL &&
                    run120.slow_count == i + 1 );
        }
    }
    assert( decision.route == WINE_PS5_WRITE_BROKER );
    assert( decision.state.mode == WINE_PS5_WRITE_THROTTLED );

    /* Agent's bulk pattern: fast <=64 KiB writes stop locally at 256 MiB. */
    for (i = 0; i < WINE_PS5_WRITE_LOCAL_LIMIT / WINE_PS5_WRITE_SMALL_MAX - 1; i++)
    {
        total = wine_ps5_title_write_local_total( total, WINE_PS5_WRITE_SMALL_MAX );
        decision = wine_ps5_title_write_route( volume, WINE_PS5_WRITE_SMALL_MAX, 1,
                                               WINE_PS5_WRITE_SMALL_MAX, total,
                                               now + i, 1, 0 );
        assert( decision.route == WINE_PS5_WRITE_LOCAL );
        volume = decision.state;
    }
    total = wine_ps5_title_write_local_total( total, WINE_PS5_WRITE_SMALL_MAX );
    decision = wine_ps5_title_write_route( volume, WINE_PS5_WRITE_SMALL_MAX, 1,
                                           WINE_PS5_WRITE_SMALL_MAX, total,
                                           now + i, 1, 0 );
    assert( decision.route == WINE_PS5_WRITE_BROKER );
    assert( decision.state.mode == WINE_PS5_WRITE_VOLUME );
    assert( decision.transition == WINE_PS5_WRITE_VOLUME_TO_BROKER );
    volume = decision.state;
    decision = wine_ps5_title_write_route( volume, 320, WINE_PS5_WRITE_LOCAL_NONE,
                                           0, total,
                                           now + 10 * WINE_PS5_WRITE_BROKER_HOLD_US, 1, 0 );
    assert( decision.route == WINE_PS5_WRITE_BROKER && !decision.probe );
    decision = wine_ps5_title_write_route( volume, 320, WINE_PS5_WRITE_LOCAL_NONE,
                                           0, total, now, 0, 0 );
    assert( decision.route == WINE_PS5_WRITE_LOCAL );
    assert( decision.state.mode == WINE_PS5_WRITE_VOLUME );
    puts( "ps5 write routing: 700-ms evidence, cumulative contention, 256-MiB cutoff, "
          "fallback and probe PASS" );
}



/* Exercise the production route/set helpers with distinct descriptors for
 * the title and broker view of one real host file. Elapsed time is injected;
 * the host test does not model Sony throttling or the TCP transport. */
static void sticky_test_write( struct wine_ps5_sticky_writes *handles,
                               struct wine_ps5_title_write_state *state, uint32_t handle,
                               int local_fd, int broker_fd, const char *bytes, size_t length,
                               off_t offset, uint64_t local_us, enum wine_ps5_title_write_route expected )
{
    struct wine_ps5_title_write_decision decision;
    unsigned int flags = wine_ps5_sticky_write_contains( handles, handle ) ? WINE_PS5_WRITE_STICKY : 0;
    ssize_t written;

    decision = wine_ps5_title_write_route( *state, length, WINE_PS5_WRITE_LOCAL_NONE,
                                           0, 0, 1000000, 1, flags );
    *state = decision.state;
    assert( decision.route == expected );
    written = pwrite( expected == WINE_PS5_WRITE_LOCAL ? local_fd : broker_fd, bytes, length, offset );
    assert( written == (ssize_t)length );
    if (expected == WINE_PS5_WRITE_LOCAL)
    {
        decision = wine_ps5_title_write_route( *state, length, local_us, written, written,
                                               1000000 + local_us, 1, flags );
        *state = decision.state;
        if (wine_ps5_sticky_write_candidate( local_us, written, 1 ))
            assert( wine_ps5_sticky_write_promote( handles, handle, "/data/anocore/cache-test" ) == 1 );
    }
}

static void test_sticky_write_routing(void)
{
    struct wine_ps5_sticky_writes handles = {0};
    struct wine_ps5_title_write_state state = WINE_PS5_TITLE_WRITE_STATE_INIT;
    struct wine_ps5_title_write_decision decision;
    char name[] = "/tmp/wine-ps5-sticky-XXXXXX", bytes[12];
    const uint32_t handle = 0x100;
    int local_fd, broker_fd;
    struct stat st;
    unsigned int i;

    assert( !wine_ps5_sticky_write_candidate( 199999, 320, 1 ) );
    assert( wine_ps5_sticky_write_candidate( 200000, 320, 1 ) );
    assert( !wine_ps5_sticky_write_candidate( 553000, 320, 0 ) );
    assert( !wine_ps5_sticky_write_candidate( 553000, -1, 1 ) );
    assert( !wine_ps5_sticky_write_candidate( 553000, 0, 1 ) );
    assert( !wine_ps5_sticky_write_promote( &handles, handle, NULL ) );
    assert( !wine_ps5_sticky_write_promote( &handles, handle, "/data/anocorex/file" ) );
    assert( !wine_ps5_sticky_write_promote( &handles, handle, "/data/anocore/" ) );
    assert( !wine_ps5_sticky_write_promote( &handles, handle, "/data/anocore//file" ) );
    assert( !wine_ps5_sticky_write_promote( &handles, handle, "/data/anocore/cache/../file" ) );
    /* the app's own folder is never brokered: the broker cannot know where it is */
    assert( !wine_ps5_sticky_write_promote( &handles, handle, "/app0/cache-test" ) );

    local_fd = mkstemp( name );
    assert( local_fd >= 0 );
    broker_fd = open( name, O_RDWR );
    assert( broker_fd >= 0 );
    /* Sticky routing remains synchronous; targeted CASC buffering is tested below. */
    sticky_test_write( &handles, &state, handle, local_fd, broker_fd, "AAAA", 4, 0, 199999,
                       WINE_PS5_WRITE_LOCAL );
    assert( !wine_ps5_sticky_write_contains( &handles, handle ) );
    assert( pread( broker_fd, bytes, 4, 0 ) == 4 && !memcmp( bytes, "AAAA", 4 ) );
    sticky_test_write( &handles, &state, handle, local_fd, broker_fd, "BBBB", 4, 4, 200000,
                       WINE_PS5_WRITE_LOCAL );
    assert( wine_ps5_sticky_write_contains( &handles, handle ) );
    assert( !wine_ps5_sticky_write_promote( &handles, handle, "/data/anocore/cache-test" ) );
    assert( !wine_ps5_sticky_write_contains( &handles, handle + 4 ) );
    assert( state.mode == WINE_PS5_WRITE_NORMAL ); /* No process-wide switch. */
    sticky_test_write( &handles, &state, handle, local_fd, broker_fd, "CCCC", 4, 0, 0,
                       WINE_PS5_WRITE_BROKER );
    sticky_test_write( &handles, &state, handle, local_fd, broker_fd, "DDDD", 4, 8, 0,
                       WINE_PS5_WRITE_BROKER );
    assert( pread( local_fd, bytes, sizeof(bytes), 0 ) == sizeof(bytes) );
    assert( !memcmp( bytes, "CCCCBBBBDDDD", sizeof(bytes) ) );
    assert( !fstat( local_fd, &st ) && st.st_size == sizeof(bytes) );
    assert( !fsync( local_fd ) ); /* Explicit flush, never replaced by an ACK. */
    assert( !close( broker_fd ) && !close( local_fd ) );
    assert( wine_ps5_sticky_write_forget( &handles, handle ) );
    assert( !wine_ps5_sticky_write_contains( &handles, handle ) );
    assert( !wine_ps5_sticky_write_forget( &handles, handle ) );
    local_fd = open( name, O_RDONLY );
    assert( local_fd >= 0 && read( local_fd, bytes, sizeof(bytes) ) == sizeof(bytes) );
    assert( !memcmp( bytes, "CCCCBBBBDDDD", sizeof(bytes) ) );
    assert( !close( local_fd ) && !unlink( name ) ); /* No delayed data at close. */

    /* The sparse set neither evicts live handles nor conflates hash collisions. */
    for (i = 0; i < 256; i++)
        assert( wine_ps5_sticky_write_promote( &handles, handle + 4 * i, "/data/anocore/cache-test" ) == 1 );
    for (i = 0; i < 256; i++) assert( wine_ps5_sticky_write_contains( &handles, handle + 4 * i ) );
    for (i = 0; i < 256; i += 2) assert( wine_ps5_sticky_write_forget( &handles, handle + 4 * i ) );
    for (i = 1; i < 256; i += 2) assert( wine_ps5_sticky_write_forget( &handles, handle + 4 * i ) );
    for (i = 0; i < WINE_PS5_WRITE_STICKY_BUCKETS; i++) assert( !handles.buckets[i] );

    /* Sticky handles retain the process policy and its cooldown/60-s probe:
     * no stuck PROBING state, and no mistaken fast probe on a fallback write. */
    state.mode = WINE_PS5_WRITE_THROTTLED;
    state.broker_until_us = 60000000;
    decision = wine_ps5_title_write_route( state, 320, WINE_PS5_WRITE_LOCAL_NONE,
                                           0, 0, 60000001, 1, WINE_PS5_WRITE_STICKY );
    assert( decision.route == WINE_PS5_WRITE_BROKER && !decision.probe );
    assert( decision.state.mode == WINE_PS5_WRITE_THROTTLED );
    decision = wine_ps5_title_write_route( state, 320, WINE_PS5_WRITE_LOCAL_NONE,
                                           0, 0, 60000001, 0, WINE_PS5_WRITE_STICKY );
    assert( decision.route == WINE_PS5_WRITE_LOCAL && !decision.probe );
    decision = wine_ps5_title_write_route( state, 320, 1000, 320, 320,
                                           60001001, 0, WINE_PS5_WRITE_STICKY );
    assert( decision.state.mode == WINE_PS5_WRITE_THROTTLED && !decision.probe );
    decision = wine_ps5_title_write_route( state, 320, WINE_PS5_WRITE_LOCAL_NONE,
                                           0, 0, 60002001, 1, 0 );
    assert( decision.route == WINE_PS5_WRITE_LOCAL && decision.probe );
    decision = wine_ps5_title_write_route( decision.state, 320, 1000, 320, 640,
                                           60003001, 1, WINE_PS5_WRITE_STICKY );
    assert( decision.route == WINE_PS5_WRITE_BROKER && decision.state.mode == WINE_PS5_WRITE_PROBING );
    puts( "ps5 sticky writes: 200-ms threshold, local-to-broker ordering, RW visibility, flush/close, "
          "collision lifetime, fallback and process probe PASS" );
}

static void test_write_trace(void)
{
    struct wine_ps5_write_trace_history history = {0}, previous;

    assert( !strcmp( wine_ps5_write_trace_class( NULL ), "unknown" ) );
    assert( !strcmp( wine_ps5_write_trace_class( "/data/anocorex/Cache/ADB/file" ), "outside-title" ) );
    assert( !strcmp( wine_ps5_write_trace_class( "/app0/wine/Cache/ADB/file" ), "outside-title" ) );
    assert( !strcmp( wine_ps5_write_trace_class( "/data/anocore/prefix/Cache/ADB/enUS/ItemSparse32.tmp" ), "title-adb" ) );
    assert( !strcmp( wine_ps5_write_trace_class( "/data/anocore/prefix/cache/adb/file" ), "title-adb" ) );
    assert( !strcmp( wine_ps5_write_trace_class( "/data/anocore/prefix/Cache/ADB2/file" ), "title-cache" ) );
    assert( !strcmp( wine_ps5_write_trace_class( "/data/anocore/prefix/Cache" ), "title-cache" ) );
    assert( !strcmp( wine_ps5_write_trace_class( "/data/anocore/prefix/Cache2/ADB/file" ), "title-other" ) );
    assert( !strcmp( wine_ps5_write_trace_class( "/data/anocore/prefix/Cac" ), "title-other" ) );

    wine_ps5_write_trace_note( &history, WINE_PS5_WRITE_TRACE_FLUSH );
    wine_ps5_write_trace_note( &history, WINE_PS5_WRITE_TRACE_EOF );
    wine_ps5_write_trace_note( &history, WINE_PS5_WRITE_TRACE_ALLOC );
    wine_ps5_write_trace_note( &history, WINE_PS5_WRITE_TRACE_READ );
    wine_ps5_write_trace_note( &history, WINE_PS5_WRITE_TRACE_SEEK );
    wine_ps5_write_trace_note( &history, WINE_PS5_WRITE_TRACE_QUERY );
    previous = wine_ps5_write_trace_take( &history );
    assert( !previous.previous_us && !previous.inflight && history.inflight == 1 );
    for (unsigned int i = 0; i < WINE_PS5_WRITE_TRACE_OPS; i++)
        assert( previous.calls[i] == 1 && !history.calls[i] );
    wine_ps5_write_trace_complete( &history, 1000, 320 );
    assert( !history.inflight && history.previous_us == 1000 && history.previous_end == 320 );

    /* Read-after-write and flush are attributed only to the next routed write,
     * never lost by a preceding snapshot. Concurrent writes are marked. */
    wine_ps5_write_trace_note( &history, WINE_PS5_WRITE_TRACE_READ );
    previous = wine_ps5_write_trace_take( &history );
    assert( previous.previous_us == 1000 && previous.previous_end == 320 );
    assert( previous.calls[WINE_PS5_WRITE_TRACE_READ] == 1 );
    wine_ps5_write_trace_note( &history, WINE_PS5_WRITE_TRACE_FLUSH );
    previous = wine_ps5_write_trace_take( &history );
    assert( previous.inflight == 1 && previous.calls[WINE_PS5_WRITE_TRACE_FLUSH] == 1 );
    assert( !previous.calls[WINE_PS5_WRITE_TRACE_READ] );
    wine_ps5_write_trace_complete( &history, 2000, 640 );
    wine_ps5_write_trace_complete( &history, 3000, -1 );
    assert( !history.inflight && history.previous_end == -1 );

    history.calls[WINE_PS5_WRITE_TRACE_SEEK] = UINT32_MAX;
    wine_ps5_write_trace_note( &history, WINE_PS5_WRITE_TRACE_SEEK );
    assert( history.calls[WINE_PS5_WRITE_TRACE_SEEK] == UINT32_MAX );
    /* The runtime resets a slot on open/close/reuse; no prior owner survives. */
    memset( &history, 0, sizeof(history) );
    previous = wine_ps5_write_trace_take( &history );
    assert( !previous.previous_us && !previous.calls[WINE_PS5_WRITE_TRACE_SEEK] );
    puts( "ps5 write trace: private path classes, operation ordering, read/flush history, overlap and reset PASS" );
}

static void test_write_coalescing(void)
{
    static const char c_root[] = "/data/anocore/prefix/drive_c";
    static const char w_root[] = "/data/anocore";
    struct wine_ps5_title_buffer_state state = {0};
    assert( WINE_PS5_TITLE_BUFFER_RUNS == 8 );
    assert( WINE_PS5_TITLE_BUFFER_TOTAL_MAX == 8u * 1024u * 1024u );

    assert( wine_ps5_title_buffer_path(
        "/data/anocore/prefix/drive_c/Games/World of Warcraft/Data/data/data.000",
        c_root, w_root ) );
    assert( wine_ps5_title_buffer_path(
        "/data/anocore/Games/World of Warcraft/_retail_/Data/data/DATA.123",
        c_root, w_root ) );
    assert( wine_ps5_title_buffer_path(
        "/data/anocore/Games/World of Warcraft/_classic_era_/Data/data/data.999",
        c_root, w_root ) );
    assert( wine_ps5_title_buffer_path(
        "/data/anocore/custom/World of Warcraft/_future_channel_/Data/data/data.042",
        c_root, w_root ) );
    assert( !wine_ps5_title_buffer_path(
        "/data/anocore/Games/World of Warcraft/Data/indices/00.idx", c_root, w_root ) );
    assert( !wine_ps5_title_buffer_path(
        "/data/anocore/Games/World of Warcraft/Data/data/data.12", c_root, w_root ) );
    assert( !wine_ps5_title_buffer_path(
        "/data/anocore/Games/World of Warcraft/Data/data/data.1234", c_root, w_root ) );
    assert( !wine_ps5_title_buffer_path(
        "/tmp/World of Warcraft/Data/data/data.000", c_root, w_root ) );

    assert( wine_ps5_title_buffer_decide( &state, 0x100, 0, 4096, 1 ) ==
            WINE_PS5_TITLE_BUFFER_START );
    state.handle = 0x100;
    state.end = 4096;
    state.length = 4096;
    assert( wine_ps5_title_buffer_decide( &state, 0x100, 4096, 16384, 1 ) ==
            WINE_PS5_TITLE_BUFFER_APPEND );
    assert( wine_ps5_title_buffer_decide( &state, 0x104, 4096, 4096, 1 ) ==
            WINE_PS5_TITLE_BUFFER_FLUSH_DIRECT );
    assert( wine_ps5_title_buffer_decide( &state, 0x100, 8192, 4096, 1 ) ==
            WINE_PS5_TITLE_BUFFER_FLUSH_DIRECT );
    assert( wine_ps5_title_buffer_decide( &state, 0x100, 4096, 4096, 0 ) ==
            WINE_PS5_TITLE_BUFFER_FLUSH_DIRECT );
    assert( !wine_ps5_title_buffer_idle( 4096, 100, 100 + WINE_PS5_TITLE_BUFFER_RESIDENCE_US - 1 ) );
    assert( wine_ps5_title_buffer_idle( 4096, 100, 100 + WINE_PS5_TITLE_BUFFER_RESIDENCE_US ) );

    assert( !wine_ps5_title_buffer_disable_for_section( 1, 0, 0 ) ); /* SEC_IMAGE */
    assert( !wine_ps5_title_buffer_disable_for_section( 0, 1, 0 ) ); /* unrelated data */
    assert( wine_ps5_title_buffer_disable_for_section( 0, 1, 1 ) );  /* direct match */
    assert( wine_ps5_title_buffer_disable_for_section( 0, 0, 0 ) );  /* unresolved */
    assert( wine_ps5_title_buffer_single_link( 1 ) );
    assert( wine_ps5_title_buffer_single_link( 0 ) ); /* Prospero unknown-count sentinel */
    assert( !wine_ps5_title_buffer_single_link( 2 ) );
    assert( wine_ps5_title_buffer_rename_may_enter_scope(
        "/data/anocore/Games/World of Warcraft/Data", c_root, w_root, 1 ) );
    assert( wine_ps5_title_buffer_rename_may_enter_scope(
        "/data/anocore/Games/Staging", c_root, w_root, 1 ) ); /* ancestor may contain WoW */
    assert( wine_ps5_title_buffer_rename_may_enter_scope(
        "/data/anocore/Games/World of Warcraft/Data/data/data.321", c_root, w_root, 0 ) );
    assert( !wine_ps5_title_buffer_rename_may_enter_scope(
        "/data/anocore/Games/World of Warcraft/Logs/log.txt", c_root, w_root, 0 ) );
    assert( !wine_ps5_title_buffer_rename_may_enter_scope(
        "/tmp/World of Warcraft/Data", c_root, w_root, 1 ) );
    assert( wine_ps5_title_buffer_same_identity( 7, 11, 7, 11 ) );
    assert( !wine_ps5_title_buffer_same_identity( 7, 11, 7, 12 ) );
    assert( !wine_ps5_title_buffer_admit( 1, 0, 1 ) ); /* shutdown recheck */
    assert( !wine_ps5_title_buffer_admit( 0, 1, 1 ) ); /* permanent disable */
    assert( wine_ps5_title_buffer_admit( 0, 0, 1 ) );
    assert( WINE_PS5_TITLE_BUFFER_EXTENTS_PER_HANDLE == 4 );
    puts( "ps5 CASC buffers: exact data.NNN scope, 8 MiB cap, four extents/handle, "
          "continuation/residence, identity, shutdown and fail-closed mapping/link policy PASS" );
}

static void test_dirquery_spin(void)
{
    struct wine_ps5_title_dirquery_spin state = {0};
    unsigned int action, i;

    for (i = 0; i < WINE_PS5_DIRQUERY_SPIN_THRESHOLD - 1; i++)
        assert( !wine_ps5_title_dirquery_spin( &state, 0x1234, 0 ) );
    action = wine_ps5_title_dirquery_spin( &state, 0x1234, 0 );
    assert( action == (WINE_PS5_DIRQUERY_SPIN_REPORT | WINE_PS5_DIRQUERY_SPIN_DELAY |
                       WINE_PS5_DIRQUERY_MITIGATION_REPORT) );
    assert( state.calls == WINE_PS5_DIRQUERY_SPIN_THRESHOLD );
    assert( wine_ps5_title_dirquery_spin( &state, 0x1234, 0 ) ==
            WINE_PS5_DIRQUERY_SPIN_DELAY );

    assert( !wine_ps5_title_dirquery_spin( &state, 0x1234, 1 ) );
    assert( state.calls == 1 );
    for (i = 1; i < WINE_PS5_DIRQUERY_SPIN_THRESHOLD - 1; i++)
        assert( !wine_ps5_title_dirquery_spin( &state, 0x1234, 1 ) );
    assert( wine_ps5_title_dirquery_spin( &state, 0x1234, 1 ) ==
            WINE_PS5_DIRQUERY_SPIN_DELAY );
    assert( state.calls == WINE_PS5_DIRQUERY_SPIN_THRESHOLD );

    assert( !wine_ps5_title_dirquery_spin( &state, 0x5678, 1 ) );
    assert( state.calls == 1 && state.reported && state.mitigation_reported );
    puts( "ps5 dirquery spin: threshold, lifetime-once markers and key-reset delay PASS" );
}

static void test_sealed_open_protection(void)
{
    int prot = PROT_READ | PROT_WRITE | PROT_EXEC;

    assert( wine_ps5_title_sealed_open_prot( prot ) == (PROT_READ | PROT_WRITE) );
    assert( wine_ps5_title_sealed_open_prot( PROT_READ | PROT_EXEC ) == PROT_READ );
    assert( wine_ps5_title_sealed_open_prot( PROT_EXEC ) == 0 );
    assert( wine_ps5_title_sealed_may_open( 0 ) );
    assert( !wine_ps5_title_sealed_may_open( 1 ) );
    puts( "ps5 sealed pages: temporary openings stay NX and execute faults synthesize neighbours PASS" );
}

static void test_virtual_copy_ranges(void)
{
    static const int readable[] = { 1, 1, 0, 1 };
    const uintptr_t start = 0x1800, page_mask = 0xfff;
    const size_t requested = 0x3000;
    size_t copied = 0, chunk;

    assert( wine_ps5_title_copy_page_size( 0x1000, 0x1800, page_mask,
                                           1, 0, 1, 1, 0 ) == 0x1000 );
    assert( wine_ps5_title_copy_page_size( 0x1ff8, 0x20, page_mask,
                                           1, 0, 1, 1, 0 ) == 8 );
    assert( !wine_ps5_title_copy_page_size( 0x1000, 0x1000, page_mask,
                                            0, 0, 1, 1, 0 ) );
    assert( !wine_ps5_title_copy_page_size( 0x1000, 0x1000, page_mask,
                                            1, 1, 1, 1, 0 ) );
    assert( !wine_ps5_title_copy_page_size( 0x1000, 0x1000, page_mask,
                                            1, 0, 0, 1, 0 ) );
    assert( !wine_ps5_title_copy_page_size( 0x1000, 0x1000, page_mask,
                                            1, 0, 1, 1, 1 ) );
    assert( wine_ps5_title_use_signal_fault_guard( 1, 1 ) );
    assert( !wine_ps5_title_use_signal_fault_guard( 1, 0 ) );
    assert( !wine_ps5_title_use_signal_fault_guard( 0, 1 ) );
    assert( !wine_ps5_title_use_signal_fault_guard( 0, 0 ) );

    while (copied < requested)
    {
        uintptr_t address = start + copied;
        size_t page = address >> 12;

        chunk = wine_ps5_title_copy_page_size( address, requested - copied, page_mask,
                                               1, 0, readable[page - 1], 1, 0 );
        if (!chunk) break;
        copied += chunk;
    }
    assert( copied == 0x1800 );  /* tail of page 1, then all of page 2 */
    puts( "ps5 self-memory copy: page/protection prefixes and fault-guard domains PASS" );
}


static void test_hang_diagnostic_helpers(void)
{
    const uint64_t constant[WINE_PS5_HANG_SNAPSHOT_COUNT] =
        { UINT64_C(0x140001000), UINT64_C(0x140001000), UINT64_C(0x140001000) };
    const uint64_t varying[WINE_PS5_HANG_SNAPSHOT_COUNT] =
        { UINT64_C(0x140001020), UINT64_C(0x140001080), UINT64_C(0x140001040) };
    const uint64_t progressing[WINE_PS5_HANG_SNAPSHOT_COUNT] =
        { UINT64_C(0x140001000), UINT64_C(0x140020000), UINT64_C(0x140080000) };
    const uint64_t base = UINT64_C(0x140000000), size = UINT64_C(0x400000);

    assert( wine_ps5_hang_classify_position( constant, WINE_PS5_HANG_VALID_MASK ) ==
            WINE_PS5_HANG_POSITION_CONSTANT );
    assert( wine_ps5_hang_classify_position( varying, WINE_PS5_HANG_VALID_MASK ) ==
            WINE_PS5_HANG_POSITION_VARYING );
    assert( wine_ps5_hang_classify_position( progressing, WINE_PS5_HANG_VALID_MASK ) ==
            WINE_PS5_HANG_POSITION_PROGRESSING );
    assert( wine_ps5_hang_classify_position( constant, WINE_PS5_HANG_VALID_MASK & ~1u ) ==
            WINE_PS5_HANG_POSITION_UNAVAILABLE );
    assert( wine_ps5_hang_classify_position(
                (const uint64_t[WINE_PS5_HANG_SNAPSHOT_COUNT]){ 0, 1, 2 },
                WINE_PS5_HANG_VALID_MASK ) == WINE_PS5_HANG_POSITION_UNAVAILABLE );
    assert( !strcmp( wine_ps5_hang_position_class_name( WINE_PS5_HANG_POSITION_VARYING ),
                     "varying" ) );

    assert( wine_ps5_hang_classify_address( base, base, size, "wowb.EXE" ) ==
            WINE_PS5_HANG_ADDRESS_WOW );
    assert( wine_ps5_wow_module( "Wow.exe" ) );
    assert( wine_ps5_wow_module( "wOwClassicT.EXE" ) );
    assert( wine_ps5_wow_module( "WowFuture.exe" ) );
    assert( !wine_ps5_wow_module( "Wow64.exe" ) );
    assert( !wine_ps5_wow_module( "Wow.exe.dll" ) );
    assert( !wine_ps5_wow_module( "BlizzardError.exe" ) );
    assert( wine_ps5_hang_classify_address( base + size - 1, base, size, "D3D12Core.dll" ) ==
            WINE_PS5_HANG_ADDRESS_SYSTEM );
    assert( wine_ps5_hang_classify_address( base + 0x1234, base, size, "gameplugin.dll" ) ==
            WINE_PS5_HANG_ADDRESS_MODULE );
    assert( wine_ps5_hang_classify_address( base + size, base, size, "ntdll.dll" ) ==
            WINE_PS5_HANG_ADDRESS_UNBACKED );
    assert( wine_ps5_hang_classify_address( base - 1, base, size, "WowB.exe" ) ==
            WINE_PS5_HANG_ADDRESS_UNBACKED );
    assert( !strcmp( wine_ps5_hang_address_class_name( WINE_PS5_HANG_ADDRESS_SYSTEM ),
                     "system" ) );
    assert( wine_ps5_hang_age_ms( 50000000, 30000000 ) == 2000 );
    assert( wine_ps5_hang_age_ms( 1, 0 ) == -1 );
    assert( wine_ps5_hang_age_ms( 9, 10 ) == -1 );
    assert( wine_ps5_hang_timeout_remaining_ms( -500000, 0, 400000 ) == 10 );
    assert( wine_ps5_hang_timeout_remaining_ms( -500000, 0, 600000 ) == 0 );
    assert( wine_ps5_hang_timeout_remaining_ms( 200000, 50000, 0 ) == 15 );
    assert( WINE_PS5_HANG_STACK_FRAMES == 24 );
    assert( WINE_PS5_HANG_MEMORY_REGS == 3 );
    assert( sizeof(struct wine_ps5_hang_registers) == 15 * sizeof(uint64_t) );
    assert( WINE_PS5_HANG_TOTAL_SNAPSHOT_COUNT == 5 );
    assert( WINE_PS5_HANG_CODE_BEFORE_BYTES == 64 );
    assert( wine_ps5_hang_sample_is_primary( 1 ) );
    assert( wine_ps5_hang_sample_is_primary( 3 ) );
    assert( !wine_ps5_hang_sample_is_primary( 0 ) );
    assert( !wine_ps5_hang_sample_is_primary( 4 ) );
    assert( wine_ps5_hang_sample_total( 2 ) == 3 );
    assert( wine_ps5_hang_sample_total( 4 ) == 5 );
    assert( WINE_PS5_PROFILE_HZ == 20 && WINE_PS5_PROFILE_WINDOW_SAMPLES == 200 );
    assert( WINE_PS5_PROFILE_WINDOW_TICKS == 201 );
    assert( WINE_PS5_PROFILE_BUCKETS == 512 && WINE_PS5_PROFILE_TOP_BUCKETS == 25 );
    assert( WINE_PS5_PROFILE_TOP_THREADS == 8 );
    assert( wine_ps5_profile_page( UINT64_C(0x1406fd5b8) ) == UINT64_C(0x1406fd000) );
    assert( wine_ps5_profile_permille( 1, 3 ) == 333 );
    assert( wine_ps5_profile_permille( 2, 3 ) == 667 );
    assert( !wine_ps5_profile_permille( 1, 0 ) );
    assert( wine_ps5_profile_hash( 1, 2 ) == wine_ps5_profile_hash( 1, 2 ) );
    assert( wine_ps5_profile_hash( 1, 2 ) != wine_ps5_profile_hash( 2, 1 ) );
    assert( wine_ps5_profile_classify( 1, 1, 1, 1 ) == WINE_PS5_PROFILE_WAIT );
    assert( wine_ps5_profile_classify( 0, 1, 1, 1 ) == WINE_PS5_PROFILE_UNIX_REQUEST );
    assert( wine_ps5_profile_classify( 0, 0, 1, 0 ) == WINE_PS5_PROFILE_USER );
    assert( wine_ps5_profile_classify( 0, 0, 0, 0 ) == WINE_PS5_PROFILE_UNIX_SYSCALL );
    assert( wine_ps5_profile_classify( 0, 0, 0, 1 ) == WINE_PS5_PROFILE_UNIX_CALL );
    assert( wine_ps5_profile_is_unix_call( 1, WINE_PS5_PROFILE_UNIX_CALL_FRAME ) );
    assert( !wine_ps5_profile_is_unix_call( 0, WINE_PS5_PROFILE_UNIX_CALL_FRAME ) );
    assert( !wine_ps5_profile_is_unix_call( 1, 0 ) );
    assert( wine_ps5_profile_cpu_permille( 100, 200, 1000, 2000 ) == 100 );
    assert( wine_ps5_profile_cpu_permille( 100, 100, 1000, 2000 ) == 0 );
    assert( wine_ps5_profile_cpu_permille( 100, 1100, 1000, 2000 ) == 1000 );
    assert( wine_ps5_profile_cpu_permille( 200, 100, 1000, 2000 ) == 0 );
    assert( wine_ps5_profile_cpu_permille( 100, 200, 2000, 2000 ) == 0 );
    assert( wine_ps5_profile_cpu_permille( 100, 200, 2000, 1000 ) == 0 );
    assert( wine_ps5_profile_cpu_permille( 0, 1, 0, 3 ) == 333 );
    assert( wine_ps5_profile_cpu_permille( 0, 2, 0, 3 ) == 667 );
    {
        /* Independent cumulative CPU baselines, one ten-second window.
         * The process sum is not capped at one CPU (100%). */
        const uint64_t first[] = { UINT64_C(100000000000), 1000, UINT64_C(900000000000) };
        const uint64_t last[] = { UINT64_C(108000000000), UINT64_C(250001000),
                                 UINT64_C(909500000000) };
        const unsigned int expected[] = { 800, 25, 950 };
        unsigned int total = 0, i;

        for (i = 0; i < 3; i++)
        {
            unsigned int cpu = wine_ps5_profile_cpu_permille(
                first[i], last[i], UINT64_C(1000000000), UINT64_C(11000000000) );
            assert( cpu == expected[i] );
            total += cpu;
        }
        assert( total == 1775 );
    }
    puts( "ps5 diagnostic helpers: hang phases, profile bucketing, timing and bounds PASS" );
}


static void test_suspend_diagnostic_marker(void)
{
    unsigned char teb[WINE_PS5_SUSPEND_DIAGNOSTIC_TEB64_OFFSET +
                      sizeof(struct wine_ps5_suspend_diagnostic)] = {0};
    struct wine_ps5_suspend_diagnostic *diagnostic =
        wine_ps5_suspend_diagnostic_from_teb64( teb );
    sigset_t critical;

    wine_ps5_title_critical_signals( &critical );
    assert( sigismember( &critical, SIGUSR1 ) );
#ifdef SIGURG
    assert( !sigismember( &critical, SIGURG ) );
#endif

    assert( (unsigned char *)diagnostic - teb == WINE_PS5_SUSPEND_DIAGNOSTIC_TEB64_OFFSET );
    assert( !wine_ps5_is_title_thread64( teb ) );
    assert( !wine_ps5_title_self_dump_disabled( 1, teb ) );
    diagnostic->magic = WINE_PS5_SUSPEND_DIAGNOSTIC_MAGIC;
    diagnostic->waiting = 1;
    diagnostic->inside_syscall = 1;
    diagnostic->native_rip = UINT64_C(0x123456789abcdef0);
    diagnostic->hang_snapshot = WINE_PS5_HANG_SNAPSHOT_COUNT;
    diagnostic->hang_epoch = 7;
    diagnostic->hang_valid = WINE_PS5_HANG_VALID_MASK;
    diagnostic->hang_rip[2] = UINT64_C(0x140001000);
    diagnostic->user_regs.rdx = UINT64_C(0x140002000);
    diagnostic->user_regs.r8 = UINT64_C(0x140003000);
    assert( wine_ps5_is_title_thread64( teb ) );
    assert( wine_ps5_title_self_dump_disabled( 1, teb ) );
    assert( !wine_ps5_title_self_dump_disabled( 0, teb ) );
    assert( diagnostic->waiting == 1 && diagnostic->inside_syscall == 1 );
    assert( diagnostic->native_rip == UINT64_C(0x123456789abcdef0) );
    assert( diagnostic->hang_snapshot == WINE_PS5_HANG_SNAPSHOT_COUNT );
    assert( diagnostic->hang_epoch == 7 && diagnostic->hang_valid == WINE_PS5_HANG_VALID_MASK );
    assert( diagnostic->hang_rip[2] == UINT64_C(0x140001000) );
    assert( diagnostic->user_regs.rdx == UINT64_C(0x140002000) );
    assert( diagnostic->user_regs.r8 == UINT64_C(0x140003000) );
    assert( !wine_ps5_present_is_hung( WINE_PS5_PRESENT_HANG_NS, 0 ) );
    assert( !wine_ps5_present_is_hung( WINE_PS5_PRESENT_HANG_NS - 1, 1 ) );
    assert( wine_ps5_present_is_hung( WINE_PS5_PRESENT_HANG_NS + 1, 1 ) );
    puts( "ps5 suspend/watchdog diagnostic: x64 TEB marker, self-dump gate and 15-s threshold PASS" );
}

static void test_hitch_counters(void)
{
    unsigned char teb[WINE_PS5_HITCH_TEB64_OFFSET + sizeof(struct wine_ps5_hitch_counters)] = {0};
    struct wine_ps5_hitch_counters *hitch = wine_ps5_hitch_from_teb64( teb );

    /* d3d12core mirrors this exact block; keep both sides in lockstep. */
    assert( WINE_PS5_HITCH_TEB64_OFFSET == 0x568 && WINE_PS5_HITCH_MAGIC == 0x48545350 );
    assert( WINE_PS5_HITCH_TEB64_OFFSET == WINE_PS5_SUSPEND_DIAGNOSTIC_TEB64_OFFSET +
                                           sizeof(struct wine_ps5_suspend_diagnostic) );
    assert( offsetof(struct wine_ps5_hitch_counters, armed) == 4 &&
            offsetof(struct wine_ps5_hitch_counters, tid) == 8 &&
            offsetof(struct wine_ps5_hitch_counters, sequence) == 12 &&
            offsetof(struct wine_ps5_hitch_counters, wait_ns) == 16 &&
            offsetof(struct wine_ps5_hitch_counters, wait_count) == 24 &&
            offsetof(struct wine_ps5_hitch_counters, read_ns) == 32 &&
            offsetof(struct wine_ps5_hitch_counters, read_count) == 40 &&
            offsetof(struct wine_ps5_hitch_counters, unwind_ns) == 48 &&
            offsetof(struct wine_ps5_hitch_counters, unwind_count) == 56 &&
            offsetof(struct wine_ps5_hitch_counters, wait_start_ns) == 64 &&
            offsetof(struct wine_ps5_hitch_counters, main_block) == 72 &&
            offsetof(struct wine_ps5_hitch_counters, cpu_ns) == 80 &&
            offsetof(struct wine_ps5_hitch_counters, cpu_sample_ns) == 88 &&
            offsetof(struct wine_ps5_hitch_counters, cpu_core) == 96 &&
            offsetof(struct wine_ps5_hitch_counters, cpu_samples) == 100 &&
            sizeof(struct wine_ps5_hitch_counters) == 104 && WINE_PS5_HITCH_NO_CORE == 0xffffffffu );
    assert( (unsigned char *)hitch - teb == WINE_PS5_HITCH_TEB64_OFFSET );
    assert( !wine_ps5_hitch_armed( hitch ) );
    hitch->armed = 1;
    assert( !wine_ps5_hitch_armed( hitch ) );
    hitch->magic = WINE_PS5_HITCH_MAGIC;
    assert( wine_ps5_hitch_armed( hitch ) );

    /* Only outermost transitions count; nested restores leave everything alone.
     * Every published update leaves the sequence even and records the CPU clock. */
    wine_ps5_hitch_blocking( hitch, 0, 1, 1000, 1, 700, 3 );
    assert( hitch->wait_start_ns == 1000 && hitch->sequence == 2 && hitch->cpu_ns == 700 &&
            hitch->cpu_sample_ns == 1000 && hitch->cpu_core == 3 && hitch->cpu_samples == 1 );
    wine_ps5_hitch_blocking( hitch, 1, 1, 1500, 1, 900, 4 );
    assert( hitch->sequence == 2 && hitch->cpu_samples == 1 );
    wine_ps5_hitch_blocking( hitch, 1, 0, 4000, 1, 710, 5 );
    assert( hitch->wait_ns == 3000 && hitch->wait_count == 1 && !hitch->wait_start_ns && hitch->sequence == 4 &&
            hitch->cpu_ns == 710 && hitch->cpu_sample_ns == 4000 && hitch->cpu_core == 5 );
    /* A wait already running when the thread was armed has no start; an
     * unreadable CPU clock keeps the previous sample. */
    wine_ps5_hitch_blocking( hitch, 1, 0, 9000, 0, 0, 0 );
    assert( hitch->wait_ns == 3000 && hitch->wait_count == 1 && hitch->sequence == 6 &&
            hitch->cpu_ns == 710 && hitch->cpu_sample_ns == 4000 && hitch->cpu_samples == 2 );
    /* A backwards clock never underflows. */
    wine_ps5_hitch_blocking( hitch, 0, 1, 5000, 1, 720, 5 );
    wine_ps5_hitch_blocking( hitch, 1, 0, 4000, 1, 720, 5 );
    assert( hitch->wait_ns == 3000 && hitch->wait_count == 2 && hitch->sequence == 10 );
    wine_ps5_hitch_add( hitch, &hitch->read_ns, &hitch->read_count, 250 );
    wine_ps5_hitch_add( hitch, &hitch->unwind_ns, &hitch->unwind_count, 40 );
    assert( hitch->read_ns == 250 && hitch->read_count == 1 && hitch->unwind_ns == 40 &&
            hitch->unwind_count == 1 && hitch->sequence == 14 );
    /* Profiler tick: records a sample, but never inside an interrupted update. */
    assert( wine_ps5_hitch_cpu_sample( hitch, 12000, 800, 6 ) && hitch->sequence == 16 &&
            hitch->cpu_ns == 800 && hitch->cpu_sample_ns == 12000 && hitch->cpu_core == 6 );
    wine_ps5_hitch_write_begin( hitch );
    assert( !wine_ps5_hitch_cpu_sample( hitch, 13000, 900, 7 ) && hitch->cpu_ns == 800 && hitch->sequence == 17 );
    wine_ps5_hitch_write_end( hitch );
    assert( hitch->sequence == 18 );

    assert( wine_ps5_hitch_exclusive( 100, 1100, 50, 350 ) == 700 );
    assert( wine_ps5_hitch_exclusive( 100, 1100, 0, 5000 ) == 0 );
    assert( wine_ps5_hitch_exclusive( 1100, 100, 0, 0 ) == 0 );
    assert( wine_ps5_hitch_exclusive( 100, 1100, 400, 300 ) == 1000 );
    puts( "ps5 hitch counters: TEB offset/layout, seqlocked outermost wait transitions, CPU samples, "
          "exclusive scopes PASS" );
}


#define ALERT_STRESS_PAIRS 12
#define ALERT_STRESS_ROUNDS 5000
struct alert_stress_pair
{
    int32_t word;
    pthread_mutex_t mutex;
    pthread_cond_t wake, progress;
    unsigned int offered, consumed, timeouts, index;
    uint64_t wakes, sleepers;
};
static struct alert_stress_pair alert_pairs[ALERT_STRESS_PAIRS];

/* Host UMTX model: test the expected word under the kernel queue mutex,
 * atomically enqueue/release it, and wake under that same mutex. */
static int alert_stress_wait( struct alert_stress_pair *pair, int infinite )
{
    struct timespec until;
    int ret = 0;

    assert( !clock_gettime( CLOCK_REALTIME, &until ) );
    until.tv_nsec += 50000;
    if (until.tv_nsec >= 1000000000) { until.tv_nsec -= 1000000000; until.tv_sec++; }
    pthread_mutex_lock( &pair->mutex );
    pthread_cond_broadcast( &pair->progress );
    if (__atomic_load_n( &pair->word, __ATOMIC_SEQ_CST ) == 2)
        ret = infinite ? pthread_cond_wait( &pair->wake, &pair->mutex ) :
                         pthread_cond_timedwait( &pair->wake, &pair->mutex, &until );
    pthread_mutex_unlock( &pair->mutex );
    return ret;
}

static void *alert_stress_waiter( void *arg )
{
    struct alert_stress_pair *pair = arg;
    unsigned int consumed = 0;

    while (consumed < ALERT_STRESS_ROUNDS)
    {
        if (wine_ps5_alert_prepare_wait( &pair->word ))
        {
            pthread_mutex_lock( &pair->mutex );
            /* One outstanding, non-coalesced alert per pair. */
            assert( pair->offered == pair->consumed + 1 );
            pair->consumed = ++consumed;
            pthread_cond_broadcast( &pair->progress );
            pthread_mutex_unlock( &pair->mutex );
        }
        else if (alert_stress_wait( pair, (pair->index & 1) && !(consumed % 5) ) == ETIMEDOUT)
        {
            wine_ps5_alert_cancel_wait( &pair->word );
            pair->timeouts++;
        }
    }
    return NULL;
}

static void *alert_stress_alerter( void *arg )
{
    struct alert_stress_pair *pair = arg;
    unsigned int i;

    for (i = 1; i <= ALERT_STRESS_ROUNDS; i++)
    {
        int wake, sleeper;

        pthread_mutex_lock( &pair->mutex );
        while (pair->consumed != i - 1)
            pthread_cond_wait( &pair->progress, &pair->mutex );
        /* Exercise both an announced sleeper and alerts arriving before wait. */
        while (!(i % 4) && __atomic_load_n( &pair->word, __ATOMIC_SEQ_CST ) != 2)
            pthread_cond_wait( &pair->progress, &pair->mutex );
        /* Exercise spurious returns on even pairs; odd pairs include infinite
         * waits without rescue broadcasts, so a missed wake trips the alarm. */
        if (!(pair->index & 1) && !(i % 64)) pthread_cond_broadcast( &pair->wake );
        pair->offered = i;
        pthread_mutex_unlock( &pair->mutex );
        wake = wine_ps5_alert_signal( &pair->word, &sleeper );
        pair->wakes += wake;
        pair->sleepers += sleeper;
        if (wake)
        {
            pthread_mutex_lock( &pair->mutex );
            pthread_cond_signal( &pair->wake );
            pthread_mutex_unlock( &pair->mutex );
        }
    }
    pthread_mutex_lock( &pair->mutex );
    while (pair->consumed != ALERT_STRESS_ROUNDS)
        pthread_cond_wait( &pair->progress, &pair->mutex );
    pthread_mutex_unlock( &pair->mutex );
    return NULL;
}


static void test_title_alerts(void)
{
    struct wine_ps5_alert_stats stats = {0}, taken;
    pthread_t waiters[ALERT_STRESS_PAIRS], alerters[ALERT_STRESS_PAIRS];
    int32_t word = 0;
    int sleeper;
    unsigned int i, timeouts = 0;
    uint64_t wakes = 0, sleepers = 0;

    assert( wine_ps5_alert_signal( &word, &sleeper ) && !sleeper && word == 1 );
    assert( !wine_ps5_alert_signal( &word, &sleeper ) && !sleeper );
    assert( wine_ps5_alert_prepare_wait( &word ) && word == 0 );
    assert( !wine_ps5_alert_prepare_wait( &word ) && word == 2 );
    assert( !wine_ps5_alert_prepare_wait( &word ) ); /* spurious wake, not an alert */
    assert( wine_ps5_alert_signal( &word, &sleeper ) && sleeper );
    wine_ps5_alert_cancel_wait( &word );
    assert( word == 1 && wine_ps5_alert_prepare_wait( &word ) );
    assert( !wine_ps5_alert_prepare_wait( &word ) );
    assert( wine_ps5_alert_signal( &word, &sleeper ) && sleeper );
    assert( wine_ps5_alert_prepare_wait( &word ) );
    assert( wine_ps5_alert_signal( &word, &sleeper ) && !sleeper );
    assert( wine_ps5_alert_prepare_wait( &word ) );
    assert( !wine_ps5_alert_prepare_wait( &word ) );
    wine_ps5_alert_cancel_wait( &word );
    assert( word == 0 );
    wine_ps5_alert_record( &stats, 1, 1, 0, 80 );
    wine_ps5_alert_record( &stats, 0, 0, 0, 20 );
    wine_ps5_alert_record( &stats, 1, 0, 1, 200 );
    taken = wine_ps5_alert_take_stats( &stats );
    assert( taken.alerts == 3 && taken.wakes == 2 && taken.sleepers == 1 );
    assert( taken.wake_errors == 1 && taken.total_ns == 300 && taken.max_ns == 200 );
    taken = wine_ps5_alert_take_stats( &stats );
    assert( !taken.alerts && !taken.wakes && !taken.max_ns && !taken.total_ns );

    for (i = 0; i < ALERT_STRESS_PAIRS; i++)
    {
        struct alert_stress_pair *pair = &alert_pairs[i];
        pair->index = i;
        assert( !pthread_mutex_init( &pair->mutex, NULL ) );
        assert( !pthread_cond_init( &pair->wake, NULL ) );
        assert( !pthread_cond_init( &pair->progress, NULL ) );
        /* A genuine timeout with no alert, before starting the producers. */
        assert( !wine_ps5_alert_prepare_wait( &pair->word ) );
        assert( alert_stress_wait( pair, 0 ) == ETIMEDOUT );
        wine_ps5_alert_cancel_wait( &pair->word );
        assert( pair->word == 0 );
        pair->timeouts++;
        assert( !pthread_create( &waiters[i], NULL, alert_stress_waiter, pair ) );
        assert( !pthread_create( &alerters[i], NULL, alert_stress_alerter, pair ) );
    }
    for (i = 0; i < ALERT_STRESS_PAIRS; i++)
    {
        assert( !pthread_join( alerters[i], NULL ) );
        assert( !pthread_join( waiters[i], NULL ) );
        assert( alert_pairs[i].offered == ALERT_STRESS_ROUNDS );
        assert( alert_pairs[i].consumed == ALERT_STRESS_ROUNDS && !alert_pairs[i].word );
        timeouts += alert_pairs[i].timeouts;
        wakes += alert_pairs[i].wakes;
        sleepers += alert_pairs[i].sleepers;
    }
    for (i = 0; i < ALERT_STRESS_PAIRS; i++)
    {
        pthread_cond_destroy( &alert_pairs[i].wake );
        pthread_cond_destroy( &alert_pairs[i].progress );
        pthread_mutex_destroy( &alert_pairs[i].mutex );
    }
    assert( sleepers && wakes >= sleepers );
    printf( "ps5 title alerts: %u pairs, %u alerts consumed exactly once, "
            "%u timeouts, %llu wakes (%llu inferred sleepers) PASS\n",
            ALERT_STRESS_PAIRS, ALERT_STRESS_PAIRS * ALERT_STRESS_ROUNDS,
            timeouts, (unsigned long long)wakes, (unsigned long long)sleepers );
}

static void test_title_alert_block(void)
{
    const size_t bytes = WINE_PS5_ALERT_BLOCK_BYTES;
    const size_t count = bytes / sizeof(struct wine_ps5_alert_entry);
    const size_t used = count * sizeof(struct wine_ps5_alert_entry);
    struct wine_ps5_alert_entry *entries;
    int sleeper;

    /* This is the production title layout (sync.c asserts the union matches).
     * Its unused tail must not be subtracted from the mapping/unmapping size:
     * that used-size allocation raised anon_mmap_alloc's PS5 assertion. */
    assert( bytes % 16384 == 0 && bytes % 4096 == 0 );
    assert( count && used <= bytes && bytes - used < sizeof(*entries) );
    assert( bytes / 40 * 40 == 65520 ); /* original diagnostic entry's failing layout */
    assert( (bytes / 40 * 40) % 16384 != 0 );
    if (bytes % sizeof(*entries)) assert( used % 16384 != 0 );
    entries = mmap( NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0 );
    assert( entries != MAP_FAILED );
    assert( !entries[0].word && !entries[count - 1].word );
    assert( !wine_ps5_alert_prepare_wait( &entries[count - 1].word ) );
    assert( wine_ps5_alert_signal( &entries[count - 1].word, &sleeper ) && sleeper );
    assert( wine_ps5_alert_prepare_wait( &entries[count - 1].word ) );
    wine_ps5_alert_announce_time( &entries[count - 1].timing, 1234 );
    assert( entries[count - 1].timing.sleep_ns == 1234 );
    assert( !munmap( entries, bytes ) );
    printf( "ps5 title alert blocks: entry=%zu bytes=%zu entries=%zu tail=%zu; "
            "4-KiB/16-KiB alignment and last-entry access PASS\n",
            sizeof(*entries), bytes, count, bytes - used );
}

static struct wine_ps5_alert_timing alert_timing_race;
static int alert_timing_done;

static void *alert_timing_writer( void *arg )
{
    unsigned int i;

    (void)arg;
    for (i = 1; i <= 100000; i++)
        wine_ps5_alert_announce_time( &alert_timing_race, (uint64_t)i * 1000 );
    __atomic_store_n( &alert_timing_done, 1, __ATOMIC_RELEASE );
    return NULL;
}

static void test_title_measurement_helpers(void)
{
    static const uint64_t bounds[] = { 2000, 5000, 10000, 20000, 50000, 100000, 500000 };
    struct wine_ps5_alert_timing timing = {0};
    struct wine_ps5_alert_stats alerts = {0}, taken_alerts;
    struct wine_ps5_read_stats reads = {0}, taken;
    struct wine_ps5_read_sample sample = {0};
    pthread_t writer;
    uint64_t sequence, ns, key;
    unsigned int i, snapshots = 0;

    for (i = 0; i < 7; i++)
    {
        assert( wine_ps5_latency_bucket( bounds[i] - 1 ) == i );
        assert( wine_ps5_latency_bucket( bounds[i] ) == i + 1 );
    }
    assert( wine_ps5_latency_bucket( UINT64_MAX ) == 7 );
    assert( !wine_ps5_alert_sleep_time( &timing, &sequence, &ns ) );
    assert( !wine_ps5_alert_return_latency( &timing, 5000, &ns ) );
    wine_ps5_alert_announce_time( &timing, 1000 );
    assert( wine_ps5_alert_sleep_time( &timing, &sequence, &ns ) && sequence == 2 && ns == 1000 );
    wine_ps5_alert_publish_time( &timing, sequence, 2000 );
    assert( wine_ps5_alert_return_latency( &timing, 5000, &ns ) && ns == 3000 );
    wine_ps5_alert_announce_time( &timing, 3000 );
    assert( !wine_ps5_alert_return_latency( &timing, 6000, &ns ) ); /* old wake, new sleeper */
    assert( wine_ps5_alert_sleep_time( &timing, &sequence, &ns ) && sequence == 4 );
    wine_ps5_alert_publish_time( &timing, sequence, 7000 );
    assert( !wine_ps5_alert_return_latency( &timing, 6000, &ns ) ); /* regressed timestamp */
    assert( wine_ps5_alert_return_latency( &timing, 10000, &ns ) && ns == 3000 );
    __atomic_store_n( &timing.sleep_sequence, 5, __ATOMIC_SEQ_CST );
    assert( !wine_ps5_alert_sleep_time( &timing, &sequence, &ns ) ); /* publication in progress */
    wine_ps5_alert_publish_time( &timing, 0, 10000 );
    assert( !wine_ps5_alert_return_latency( &timing, 20000, &ns ) );

    assert( !pthread_create( &writer, NULL, alert_timing_writer, NULL ) );
    do
    {
        if (wine_ps5_alert_sleep_time( &alert_timing_race, &sequence, &ns ))
        {
            assert( ns == sequence / 2 * 1000 );
            snapshots++;
        }
    } while (!__atomic_load_n( &alert_timing_done, __ATOMIC_ACQUIRE ));
    assert( !pthread_join( writer, NULL ) );
    assert( wine_ps5_alert_sleep_time( &alert_timing_race, &sequence, &ns ) );
    assert( sequence == 200000 && ns == 100000000 );
    alerts.gap[0] = 2; alerts.gap[7] = 3; alerts.gap_unavailable = 4;
    alerts.wake[3] = 5; alerts.wake_unavailable = 6;
    taken_alerts = wine_ps5_alert_take_stats( &alerts );
    assert( taken_alerts.gap[0] == 2 && taken_alerts.gap[7] == 3 && taken_alerts.gap_unavailable == 4 );
    assert( taken_alerts.wake[3] == 5 && taken_alerts.wake_unavailable == 6 );
    taken_alerts = wine_ps5_alert_take_stats( &alerts );
    assert( !taken_alerts.gap[0] && !taken_alerts.gap[7] && !taken_alerts.wake[3] &&
            !taken_alerts.gap_unavailable && !taken_alerts.wake_unavailable );

    assert( wine_ps5_read_size_bucket( 4095 ) == 0 && wine_ps5_read_size_bucket( 4096 ) == 1 );
    assert( wine_ps5_read_size_bucket( 65535 ) == 1 && wine_ps5_read_size_bucket( 65536 ) == 2 );
    assert( wine_ps5_read_size_bucket( 1048575 ) == 2 && wine_ps5_read_size_bucket( 1048576 ) == 3 );
    assert( wine_ps5_read_directory_class( NULL ) == WINE_PS5_READ_UNKNOWN );
    assert( wine_ps5_read_directory_class( "/Game/Data/data/data.003" ) == WINE_PS5_READ_CASC );
    assert( wine_ps5_read_directory_class( "C:\\Game\\dAtA\\DaTa\\data.004" ) == WINE_PS5_READ_CASC );
    assert( wine_ps5_read_directory_class( "/Game/Metadata/data.003" ) == WINE_PS5_READ_OTHER );
    assert( wine_ps5_read_directory_class( "Data/datafile/a" ) == WINE_PS5_READ_OTHER );
    assert( wine_ps5_read_directory_class( "D" ) == WINE_PS5_READ_OTHER );
    key = wine_ps5_read_class_key( 0x24, WINE_PS5_READ_CASC );
    assert( wine_ps5_read_class_from_key( key, 0x24 ) == WINE_PS5_READ_CASC );
    assert( wine_ps5_read_class_from_key( key, 0x28 ) == WINE_PS5_READ_UNKNOWN );
    assert( wine_ps5_read_class_from_key( 0, 0x24 ) == WINE_PS5_READ_UNKNOWN );
    sample.bytes = 2048; sample.lookup_ns = 100; sample.pread_ns = 400; sample.native_calls = 1;
    sample.synchronous = 1;
    wine_ps5_read_record( &reads, &sample, 4096, 1000, WINE_PS5_READ_CASC, 0, 0 );
    memset( &sample, 0, sizeof(sample) );
    sample.bytes = 65536; sample.read_ns = 200; sample.native_calls = 1; sample.unknown_backend = 1;
    wine_ps5_read_record( &reads, &sample, 65536, 4000, WINE_PS5_READ_UNKNOWN, 1, 1 );
    taken = wine_ps5_read_take_stats( &reads );
    assert( taken.calls == 2 && taken.bytes == 67584 && taken.total_ns == 5000 && taken.max_ns == 4000 );
    assert( taken.lookup_ns == 100 && taken.pread_ns == 400 && taken.read_ns == 200 && taken.native_calls == 2 );
    assert( taken.sync_calls == 1 && taken.pending_calls == 1 && taken.failed_calls == 1 &&
            taken.unknown_backend == 1 && taken.sizes[1] == 1 && taken.sizes[2] == 1 );
    assert( taken.classes[WINE_PS5_READ_CASC] == 1 && taken.classes[WINE_PS5_READ_UNKNOWN] == 1 );
    taken = wine_ps5_read_take_stats( &reads );
    assert( !taken.calls && !taken.bytes && !taken.total_ns && !taken.max_ns &&
            !taken.sizes[1] && !taken.classes[WINE_PS5_READ_CASC] );
    printf( "ps5 title measurements: gap/wake bounds and epochs, %u coherent concurrent snapshots, "
            "read bytes/timing/sizes/classes/reset PASS\n", snapshots );
}

static uint64_t qpc_exact_ticks( uint64_t tsc, uint64_t hz )
{
    return (uint64_t)((unsigned __int128)tsc * WINE_PS5_QPC_TICKS_PER_SEC / hz);
}

/* Simulated clocks: true time in ns; CLOCK_MONOTONIC may run ppm off the TSC. */
static uint64_t qpc_sim_tsc( uint64_t ns, uint64_t hz )
{
    return UINT64_C(5000000000000) + (uint64_t)((unsigned __int128)ns * hz / 1000000000);
}

static uint64_t qpc_sim_mono( uint64_t ns, int64_t ppm )
{
    return UINT64_C(123456789) + (uint64_t)(((__int128)ns * (1000000 + ppm)) / 1000000 / 100);
}

static void test_qpc_from_tsc(void)
{
    static const uint64_t rates[] = { 1596292266, 1600000000, 2000000000, 3200000000, 4000000000,
                                      WINE_PS5_QPC_TICKS_PER_SEC + 1, UINT64_C(0xfffffffffffffff1) };
    static const int64_t ppms[] = { 0, 50, -50, 1000 };
    const uint64_t month = UINT64_C(30) * 86400;
    struct wine_ps5_qpc_state state;
    uint64_t hz, mul, tsc, last, ticks, bias, mono, ns, before, after, value, previous, span, seed = 1;
    unsigned __int128 exact;
    unsigned int i, j, switches = 0, readings = 0;
    int64_t drift;
    int mode;

    assert( !wine_ps5_qpc_multiplier( 0 ) );
    assert( !wine_ps5_qpc_multiplier( WINE_PS5_QPC_TICKS_PER_SEC ) );
    for (i = 0; i < sizeof(rates) / sizeof(rates[0]); i++)
    {
        hz = rates[i];
        mul = wine_ps5_qpc_multiplier( hz );
        /* round(2^64 * 10^7 / hz), checked with a real 128-bit divide */
        exact = ((unsigned __int128)WINE_PS5_QPC_TICKS_PER_SEC << 64) + hz / 2;
        assert( mul == (uint64_t)(exact / hz) );
        /* At most one tick from the exact floor across the whole 64-bit TSC
         * range, half of the samples within 30 days of uptime, non-decreasing. */
        span = hz <= 4000000000 ? month * hz : UINT64_MAX;
        for (j = 0; j < 200000; j++)
        {
            seed = seed * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
            tsc = j < 100000 && span < UINT64_MAX ? seed % (span + 1) : seed;
            if (j == 0) tsc = 0;
            if (j == 1) tsc = UINT64_MAX;
            if (j == 2) tsc = span;
            ticks = wine_ps5_qpc_scale( tsc, mul );
            exact = qpc_exact_ticks( tsc, hz );
            assert( ticks + 1 >= exact && ticks <= exact + 1 );
            if (tsc < UINT64_MAX) assert( wine_ps5_qpc_scale( tsc + 1, mul ) >= ticks );
        }
        if (hz > 4000000000) continue;
        /* 30 days of uptime stays far below int64 ticks, so biasing cannot wrap. */
        assert( wine_ps5_qpc_scale( month * hz, mul ) < UINT64_C(1) << 62 );
        /* monotonic over consecutive TSC values across a long stretch */
        last = wine_ps5_qpc_scale( month * hz - 2000000, mul );
        for (tsc = month * hz - 1999999; tsc <= month * hz; tsc++)
        {
            ticks = wine_ps5_qpc_scale( tsc, mul );
            assert( ticks >= last && ticks - last <= 1 );
            last = ticks;
        }
        /* bias alignment: positive and negative (wrapping) biases */
        tsc = UINT64_C(987654321987);
        for (mono = 0; mono < UINT64_C(3) << 40; mono += UINT64_C(1) << 40)
        {
            bias = wine_ps5_qpc_bias( mono, tsc, mul );
            assert( wine_ps5_qpc_scale( tsc, mul ) + bias == mono );
            ticks = wine_ps5_qpc_scale( tsc + hz, mul ) + bias - mono;
            assert( ticks + 1 >= WINE_PS5_QPC_TICKS_PER_SEC && ticks <= WINE_PS5_QPC_TICKS_PER_SEC + 1 );
        }
        /* zero drift against a matching CLOCK_MONOTONIC over 30 days */
        memset( &state, 0, sizeof(state) );
        state.mul = mul;
        state.calibration_bias = state.tsc_bias =
            wine_ps5_qpc_bias( qpc_sim_mono( 0, 0 ), qpc_sim_tsc( 0, hz ), mul );
        for (ns = 0; ns <= month * 1000000000; ns += month * 1000000000 / 64)
        {
            drift = wine_ps5_qpc_drift( &state, qpc_sim_tsc( ns, hz ), qpc_sim_mono( ns, 0 ),
                                        qpc_sim_tsc( ns, hz ) );
            assert( drift >= -2 && drift <= 2 );
        }
        /* 50 ppm fast clock: 10 s windows show the 500 us drift */
        drift = wine_ps5_qpc_drift( &state, qpc_sim_tsc( UINT64_C(10000000000), hz ),
                                    qpc_sim_mono( UINT64_C(10000000000), 50 ), qpc_sim_tsc( UINT64_C(10000000000), hz ) );
        assert( drift >= -5002 && drift <= -4998 );
    }

    /* Switching: OFF ignores requests; no TSC refuses; QPC never runs backwards. */
    memset( &state, 0, sizeof(state) );
    assert( wine_ps5_qpc_select( &state, WINE_PS5_QPC_TSC, 0, 0, 0 ) == WINE_PS5_QPC_OFF );
    state.mode = WINE_PS5_QPC_SYSCALL;
    assert( wine_ps5_qpc_select( &state, WINE_PS5_QPC_TSC, 0, 1000, 0 ) == WINE_PS5_QPC_SYSCALL );
    assert( !state.syscall_offset && !state.tsc_bias );
    for (i = 0; i < sizeof(ppms) / sizeof(ppms[0]); i++)
    {
        hz = 1596292266;
        memset( &state, 0, sizeof(state) );
        state.mul = wine_ps5_qpc_multiplier( hz );
        state.calibration_bias = state.tsc_bias =
            wine_ps5_qpc_bias( qpc_sim_mono( 0, ppms[i] ), qpc_sim_tsc( 0, hz ), state.mul );
        state.mode = WINE_PS5_QPC_SYSCALL;
        previous = 0;
        ns = 0;
        for (j = 0; j < 20000; j++)
        {
            seed = seed * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
            ns += (seed >> 33) % 50000000;  /* up to 50 ms between calls */
            if (j % 97 == 96)
            {
                /* pair with 2-40 us clock latency; the clock read lands mid-pair */
                before = qpc_sim_tsc( ns, hz );
                ns += 1000 + (seed >> 20) % 19000;
                mono = qpc_sim_mono( ns, ppms[i] );
                ns += 1000 + (seed >> 40) % 19000;
                after = qpc_sim_tsc( ns, hz );
                mode = state.mode;
                assert( wine_ps5_qpc_select( &state, mode == WINE_PS5_QPC_TSC ? WINE_PS5_QPC_SYSCALL :
                                             WINE_PS5_QPC_TSC, before, mono, after ) != mode );
                mode = state.mode;
                assert( wine_ps5_qpc_select( &state, mode, before, mono, after ) == mode );
                switches++;
            }
            if (state.mode == WINE_PS5_QPC_TSC) value = wine_ps5_qpc_from_tsc( &state, qpc_sim_tsc( ns, hz ) );
            else value = wine_ps5_qpc_from_syscall( &state, qpc_sim_mono( ns, ppms[i] ) );
            assert( value >= previous );
            previous = value;
            readings++;
        }
        /* Steps stay bounded by pair latency plus accumulated ppm drift. */
        assert( state.syscall_offset < (uint64_t)(ns / 100 / 1000000 * (ppms[i] < 0 ? -ppms[i] : ppms[i])) +
                                           UINT64_C(206) * 400 );
    }
    printf( "ps5 title qpc: multiplier, <=1 tick over 30-day and 64-bit TSC ranges, no overflow, bias "
            "alignment, drift, %u switches/%u readings monotonic PASS\n", switches, readings );
}

int main(void)
{
    test_cursor_message();
    alarm( 30 );  /* a deadlock is a bounded failure, not an indefinitely hung test */
    test_thread_registry();
    test_supervisor( 0 );
    test_supervisor( 1 );
    test_callback_quiescence();
    test_initial_thread_quiescence();
    test_received_directory();
    test_native_exit_with_cache_lock();
    test_quit_during_directory_operation();
    test_lifetime_tickets();
    test_startup_cancellation();
    test_directory_output();
    test_directory_fault();
    test_radv_windows();
    test_direct_cache_image_gate();
    test_title_driver_load();
    test_title_stdout_binding();
    test_generic_identity();
    test_sdr_white_level_conversion();
    test_hdr_luminance_edid();
    test_game_image_rule();
    test_refused_program_rule();
    test_write_routing();
    test_sticky_write_routing();
    test_write_coalescing();
    test_write_trace();
    test_sealed_open_protection();
    test_virtual_copy_ranges();
    test_dirquery_spin();
    test_hang_diagnostic_helpers();
    test_suspend_diagnostic_marker();
    test_hitch_counters();
    test_title_alerts();
    test_title_alert_block();
    test_title_measurement_helpers();
    test_qpc_from_tsc();
    alarm( 0 );
    puts( "ps5 title lifecycle: registry, supervisor, startup, lifetime and directory regressions passed" );
    return 0;
}
