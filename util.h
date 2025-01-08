#pragma once

#include "asgn2_helper_funcs.h"
#include "connection.h"
#include "debug.h"
#include "response.h"
#include "request.h"
#include "queue.h"

#include <assert.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include <sys/stat.h>
#include <sys/file.h>
pthread_mutex_t send_mutex;

void conn_audit(conn_t *conn, const Response_t *res) {

    // <OPERATION>,<URI>,<STATUS-CODE>,<REQUEST ID HEADER VALUE>\n
    fprintf(stderr, "%s,%s,%d,%s\n", response_get_message(res), conn_get_uri(conn),
        response_get_code(res), conn_get_header(conn, "Request-Id"));
}

void send(conn_t *conn, const Response_t *res) {
    pthread_mutex_lock(&send_mutex);
    conn_send_response(conn, res);
    conn_audit(conn, res); //audit
    pthread_mutex_unlock(&send_mutex);
}
