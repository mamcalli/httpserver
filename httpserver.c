//ASGN 4 Michael McAllister, mamcalli
// starter code from:
// httpserver.c:
//   Asgn 2: A simple HTTP server.
//   By: Eugene Chou
//       Andrew Quinn
//       Brian Zhao

#include "util.h"
#include "asgn2_helper_funcs.h"
#include "connection.h"
//#include "debug.h"
#include "response.h"
#include "request.h"
#include "queue.h"

void handle_connection(uintptr_t);

void handle_get(conn_t *);
void handle_put(conn_t *);
void handle_unsupported(conn_t *);
void conn_audit(conn_t *, const Response_t *);

void send(conn_t *, const Response_t *);
//void * listener_thread(void *args);
void *worker_threads(void *args);

pthread_mutex_t f_mutex;

//Global queue for storing connection requests
queue_t *qu;

int main(int argc, char **argv) {

    size_t port;
    int opt;
    int thread_ct = 4; //default thread count

    // check -t threads option
    // Parse command line arguments
    while ((opt = getopt(argc, argv, "t:")) != -1) {
        switch (opt) {
        case 't': thread_ct = atoi(optarg); break;
        default: warnx("wrong options: %s -t threads", argv[0]); return EXIT_FAILURE;
        }
    }

    // Check if port number is provided
    // if there are no args after -t threads, error
    if (optind >= argc) {
        warnx("wrong arguments: %s port_num", argv[0]);
        fprintf(stderr, "usage: %s [-t threads] <port>\n", argv[0]);
        return EXIT_FAILURE;
        // otherwise: set port
    } else {
        // Parse port number
        char *endptr = NULL;
        port = (size_t) strtoull(argv[optind], &endptr, 10);

        if (endptr && *endptr != '\0') {
            warnx("invalid port number: %s", argv[1]);
            return EXIT_FAILURE;
        }
    }

    // initialize global lock/mutexes
    int rc = pthread_mutex_init(&f_mutex, NULL);
    int rd = pthread_mutex_init(&send_mutex, NULL);

    assert(!rc);
    assert(!rd);

    // initialize queue with thread_ct threads
    qu = queue_new(thread_ct);

    // create worker threads
    pthread_t threads[thread_ct];
    for (int i = 0; i < thread_ct; ++i) {
        pthread_create(threads + i, NULL, worker_threads, NULL);
    }

    // Ignore SIGPIPE signal so server continues running if one connection 
    // closes unexpectedly. Instead, write ops fail with EPIPE error
    signal(SIGPIPE, SIG_IGN);
    // Initialize and bind listener socket to port to listen for incoming client connections
    Listener_Socket sock;
    listener_init(&sock, port);

    // dispatcher
    while (1) {
        intptr_t connfd = listener_accept(&sock);
        queue_push(qu, (void *) connfd);
        //handle_connection(connfd);
        //close(connfd);
    }

    // error handling incase connection unexpectedly closes
    rc = pthread_mutex_destroy(&f_mutex);
    rc = pthread_mutex_destroy(&send_mutex);

    assert(!rc);
    queue_delete(&qu);
    return EXIT_SUCCESS;
}

void handle_connection(uintptr_t connfd) {

    conn_t *conn = conn_new(connfd);

    const Response_t *res = conn_parse(conn);

    if (res != NULL) {
        send(conn, res);
        //        conn_send_response(conn, res);
        //        conn_audit(conn, res);
    } else {
        //debug("%s", conn_str(conn));
        const Request_t *req = conn_get_request(conn);
        if (req == &REQUEST_GET) {
            handle_get(conn);
        } else if (req == &REQUEST_PUT) {
            handle_put(conn);
        } else {
            handle_unsupported(conn);
        }
    }

    conn_delete(&conn);
    close(connfd);
}

void handle_get(conn_t *conn) {

    char *uri = conn_get_uri(conn);
    const Response_t *res = NULL;
    //debug("handling get request for %s", uri);

    pthread_mutex_lock(&f_mutex);
    // 1. Open the file.
    int ofile = open(uri, O_RDONLY, 0666);

    // If  open it returns < 0, then use the result appropriately
    if (ofile < 0) {
        if (errno == EACCES) {
            res = &RESPONSE_FORBIDDEN;
            send(conn, res);
            pthread_mutex_unlock(&f_mutex);
            return;
            //b. Cannot find the file -- use RESPONSE_NOT_FOUND
        } else if (errno == ENOENT) {
            res = &RESPONSE_NOT_FOUND;
            send(conn, res);
            pthread_mutex_unlock(&f_mutex);
            return;
            //c. other error? -- use RESPONSE_INTERNAL_SERVER_ERROR
        } else {
            res = &RESPONSE_INTERNAL_SERVER_ERROR;
            send(conn, res);
            pthread_mutex_unlock(&f_mutex);
            return;
        }
    }

    //place writer lock & lift file lock
    flock(ofile, LOCK_SH);
    pthread_mutex_unlock(&f_mutex);

    // 2. Get the size of the file.
    struct stat f_stat;
    fstat(ofile, &f_stat);
    int f_size = f_stat.st_size;

    // 3. Check if the file is a directory, because directories *will*
    // open, but are not valid.
    if (S_ISDIR(f_stat.st_mode)) {
        // INTERNAL_SERVER_ERR
        res = &RESPONSE_FORBIDDEN;
        send(conn, res);

        close(ofile);
        return;
    }

    // If no error encountered, OK
    if (res == NULL) {
        res = &RESPONSE_OK;
    }

    pthread_mutex_lock(&send_mutex);
    // Send file contents
    res = conn_send_file(conn, ofile, f_size);
    // Check if successful
    if (res == NULL) {
        res = &RESPONSE_OK;
    }
    // Log results
    conn_audit(conn, res);
    pthread_mutex_unlock(&send_mutex);

    flock(ofile, LOCK_UN);
    close(ofile);
}

void handle_unsupported(conn_t *conn) {
    //debug("handling unsupported request");
    // send responses
    conn_send_response(conn, &RESPONSE_NOT_IMPLEMENTED);
    conn_audit(conn, &RESPONSE_NOT_IMPLEMENTED); //audit
}

void handle_put(conn_t *conn) {
    // Retrieve URI
    char *uri = conn_get_uri(conn);
    const Response_t *res = NULL;
    //debug("handling put request for %s", uri);

    // LOCK
    pthread_mutex_lock(&f_mutex);
    // Check if file already exists before opening it.
    bool existed = access(uri, F_OK) == 0;
    //debug("%s existed? %d", uri, existed);

    // Open the file..
    int fd = open(uri, O_CREAT | O_WRONLY, 0600);
    if (fd < 0) {
        //debug("%s: %d", uri, errno);
        if (errno == EACCES || errno == EISDIR || errno == ENOENT) {
            res = &RESPONSE_FORBIDDEN;
            send(conn, res);
            pthread_mutex_unlock(&f_mutex);

            return;
            //goto out;
        } else {
            res = &RESPONSE_INTERNAL_SERVER_ERROR;
            send(conn, res);
            pthread_mutex_unlock(&f_mutex);

            //goto out;
            return;
        }
    }

    // apply file lock, clear file
    flock(fd, LOCK_EX);
    ftruncate(fd, 0);
    pthread_mutex_unlock(&f_mutex);
    // write data into file
    res = conn_recv_file(conn, fd);

    if (res == NULL && existed) {
        res = &RESPONSE_OK;
    } else if (res == NULL && !existed) {
        res = &RESPONSE_CREATED;
    }

    send(conn, res);
    flock(fd, LOCK_UN);
    close(fd);

    //out:
    //
    //    conn_send_response(conn, res);
    //    conn_audit(conn, res); //audit
}

void *worker_threads(void *args) {
    (void) *args;
    while (1) {
        uintptr_t conn;
        queue_pop(qu, (void **) &conn);
        //pthread_mutex_lock(&mutex);
        handle_connection((int) conn);
        //pthread_mutex_unlock(&mutex);
        //close(conn);
    }
    return NULL;
}
