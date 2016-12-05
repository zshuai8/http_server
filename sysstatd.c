#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <signal.h>
#include <sys/wait.h>
#include <pthread.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "list.h"
#include "rio.h"
#include "threadpool.h"

#define THREADS 50
#define MAXLINE 8192
#define MAXBUF 8192
#define LISTENQ 1024

#define STATIC 0
#define DYNAMIC 1
#define LOADAVG 2
#define MEMINFO 3
#define RUNLOOP 4
#define ALLOCANON 5
#define FREEANON 6

extern char **environ;
static struct thread_pool *pool;
static char *path;
struct list memory_list;

struct memory {
    void *block;
    struct list_elem elem;
};

// When client request a file or a excutable which doesn't exist. use this for error
// This will send a html back to client and explain the error
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg, char *version);

// Responde a http request
void doit(struct thread_pool *pool, void *data);

// Read and ignore request headers
void read_requesthdrs(rio_t *rp);

// Parse uri into file name and cgiargs. For both static and dynamic
// If it is static, filename will be the path of that file in server, cgiargs will be ""
// If it is dynamic, filename will be the path of that cgi file in the server, cgiargs will be arguments of the excutable
// If it is /loadavg or /meminfo, filename will be "", cgiargs will be the callback function
// If it is /runloop, /alloanno or /freeannon, filename will be "", cigargs will be ""
int parse_uri(char *uri, char *filename, char *cgiargs);

// Seve static request
void serve_static(int fd, char *filename, int filesize);

// Serve dynamic request
void serve_dynamic(int fd, char *filename, char *cgiargs);

// Get the filetype from filename for Content-Type in the response header
void get_filetype(char *filename, char *filetype);

// Send a reponse to client with msg, content_type, version
void send_response(int fd, char *msg, char *content_type, char *version);

// The function of /runloop
static void run_loop(void);

// Helper function for listen file descriptor
static int open_listenfd(char *port);
int Open_listenfd(char *port);

static void usage(char *programme) {
    printf("Usage: %s -h\n"
           " -h Show help\n"
           " -p port to accept HTTP requests from clients\n"
           " -R specify root directory for server under '/files' prefix\n",
           programme);
    exit(0);
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);

    int listenfd, connfd;
    // char hostname[MAXLINE];
    char port[MAXLINE];
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    // char *port;

    if (argc == 1) {
        usage(argv[0]);
    }

    // To read the option and get the port and default path
    char c;
    while ((c = getopt(argc, argv, "p:R:")) != -1) {
        switch (c) {
            case 'h': {
                usage(argv[0]);
                break;
            }
            case 'p': {
                strcpy(port, strdup(optarg));
                // port = strdup(optarg);

                if (errno == ERANGE) {
                    printf("The post number is out of range");
                    return -1;
                }
                if (errno == EINVAL) {
                    printf("The base is not supported");
                    return -1;
                }
                break;
            }
            case 'R': {
                path = strdup(optarg);
                break;
            }
            default: { usage(argv[0]); }
        }
    }

    // Create thread pool for request
    pool = thread_pool_new(THREADS);

    // Init the memory list for allocate and free
    list_init(&memory_list);

    if (path == NULL) {
        path = "./files";
    }

    // Start to listen to one port
    listenfd = Open_listenfd(port);

    while (1) {
        // Accept the connection and get file descriptor
        clientlen = sizeof(clientaddr);

        if ((connfd = accept(listenfd, (struct sockaddr *)&clientaddr, &clientlen)) < 0) {
            fprintf(stderr, "Error accepting connection.\n");
            exit(1);
        }
        int *connfd_ptr = &connfd;
        thread_pool_submit(pool, (fork_join_task_t)doit, connfd_ptr);
    }
    thread_pool_shutdown_and_destroy(pool);
    return 0;
}

// Process one http request
void doit(struct thread_pool *pool, void *data) {
    int uri_type;
    struct stat sbuf;
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char filename[MAXLINE], cgiargs[MAXLINE];
    rio_t rio;

    int fd = *(int *)data;

    // Start to read the http request
    Rio_readinitb(&rio, fd);

    // while (1) {
    //     ssize_t read =

    while (1) {

        ssize_t read = Rio_readlineb(&rio, buf, MAXLINE);

        if (read <= 0) {
            break;
        }

        sscanf(buf, "%s %s %s", method, uri, version);

        // If the uri is /, cat files/
        if (strcmp(uri, "/") == 0) {
            strcat(uri, "files/");
        }

        if (strcasecmp(method, "GET")) {
            clienterror(fd, method, "501", "Not implemented", "Sysstatd Web server doesn't implement this method", version);
            close(fd);
            return;
        }

        read_requesthdrs(&rio);

        if ((uri_type = parse_uri(uri, filename, cgiargs)) < 0) {
            clienterror(fd, filename, "404", "Not found", "Sysstatd Web server couldn't find this file", version);
            close(fd);
            return;
        }

        if (uri_type == STATIC || uri_type == DYNAMIC) {
            if (stat(filename, &sbuf) < 0) {
                clienterror(fd, filename, "404", "Not found", "Sysstatd Web server couldn't find this file", version);
                close(fd);

                return;
            }

            if (strstr(filename, "..") != NULL) {
                clienterror(fd, filename, "403", "Forbidden", "Sysstatd Web Server couldn't read the file", version);
                close(fd);

                return;
            }

            if (uri_type == STATIC) {
                if (!(S_ISREG(sbuf.st_mode)) || !(S_IRUSR & sbuf.st_mode)) {
                    clienterror(fd, filename, "403", "Forbidden", "Sysstatd Web server couldn’t read the file", version);
                    close(fd);

                    return;
                }
                serve_static(fd, filename, sbuf.st_size);
            } else if (uri_type == DYNAMIC) {
                if (!(S_ISREG(sbuf.st_mode)) || !(S_IXUSR & sbuf.st_mode)) {
                    clienterror(fd, filename, "403", "Forbidden", "Sysstatd Web server couldn’t run the CGI program", version);
                    close(fd);

                    return;
                }
                serve_dynamic(fd, filename, cgiargs);
            }
        } else if (uri_type == LOADAVG) {
            // cigargs is the callback function
            FILE *fp = fopen("/proc/loadavg", "r");
            if (fp) {
                char buf[256];
                fgets(buf, sizeof(buf), fp);

                float utilization0, utilization1, utilization2;
                int running;
                char separate;
                int total;

                sscanf(buf, "%f %f %f %d %c %d", &utilization0, &utilization1, &utilization2, &running, &separate, &total);

                char return_json[256];
                sprintf(return_json,
                        "{\"total_threads\": \"%d\", \"loadavg\": [\"%.2f\", \"%.2f\", \"%.2f\"], \"running_threads\": \"%d\"}",
                        total, utilization0, utilization1, utilization2, running);

                if (strlen(cgiargs) != 0) { // has callback
                    char callback_buf[256];
                    sprintf(callback_buf, "%s(%s)", cgiargs, return_json);
                    send_response(fd, callback_buf, "application/javascript", version);
                } else {
                    send_response(fd, return_json, "application/json", version);
                }

            } else {
                clienterror(fd, filename, "403", "Forbidden", "Sysstatd Web Server couldn't read the file", version);
            }
            fclose(fp);

        } else if (uri_type == MEMINFO) {
            // cgiargs is the callback function

            FILE *fp = fopen("/proc/meminfo", "r");
            if (fp) {
                char line[128];
                char mem_info[1024];
                strcpy(mem_info, "{");
                int begin = 1;

                while (fgets(line, sizeof(line), fp)) {
                    if (!begin) {
                        strcat(mem_info, ",");
                    }

                    char key[64];
                    long value;
                    char kb[8];
                    char json_line[128];

                    sscanf(line, "%s %lu %s", key, &value, kb);
                    key[strlen(key) - 1] = '\0';
                    sprintf(json_line, "\"%s\": \"%lu\"", key, value);
                    strcat(mem_info, json_line);
                    begin = 0;
                }
                strcat(mem_info, "}");

                if (strlen(cgiargs) != 0) { // has callback
                    char callback_buf[256];
                    sprintf(callback_buf, "%s(%s)", cgiargs, mem_info);
                    send_response(fd, callback_buf, "application/javascript", version);
                } else {
                    send_response(fd, mem_info, "application/json", version);
                }
            } else {
                clienterror(fd, filename, "403", "Forbidden", "Sysstatd Web Server couldn't read the file", version);
            }
            fclose(fp);

        } else if (uri_type == RUNLOOP) {
            send_response(fd, "<html>\n<body>\n<p>Started 15 second's loop.</p>\n</body>\n</html>", "text/html", version);
            thread_pool_submit(pool, (fork_join_task_t)run_loop, (int *)0);
        } else if (uri_type == ALLOCANON) {
            void *mem_block = mmap(NULL, 268435456, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (mem_block == MAP_FAILED) {
                fprintf(stderr, "mmap() failed %s\n", strerror(errno));
            } else {
                struct memory *memory_struct = (struct memory *)malloc(sizeof(struct memory));
                memory_struct->block = mem_block;
                list_push_back(&memory_list, &memory_struct->elem);
                send_response(fd, "<html>\n<body>\n<p>Allocated 256Mb memory.</p>\n</body>\n</html>", "text/html", version);
            }
        } else if (uri_type == FREEANON) {
            if (list_size(&memory_list) > 0) {
                struct list_elem *e = list_pop_back(&memory_list);
                struct memory *mem = list_entry(e, struct memory, elem);

                if (munmap(mem->block, 268435456) == 0) {
                    send_response(fd, "<html>\n<body>\n<p>Freed 256Mb memory.</p>\n</body>\n</html>", "text/html", version);
                } else {
                    fprintf(stderr, "munmap() failed %s\n", strerror(errno));
                }

            } else {
                send_response(fd, "<html>\n<body>\n<p>No memory to free.</p>\n</body>\n</html>", "text/html", version);
            }
        }
        if (strncmp(version, "HTTP/1.0", 8) == 0) {
            break;
        }
    }
    close(fd);
}

void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg, char *version) {
    char buf[MAXLINE], body[MAXBUF];

    /* Print the HTTP response */
    sprintf(buf, "%s %s %s\r\n", version, errnum, shortmsg);
    Rio_writen(fd, buf, strlen(buf));

    sprintf(buf, "Content-type: text/html\r\n");
    Rio_writen(fd, buf, strlen(buf));

    sprintf(buf, "Content-length: %d\r\n\r\n", (int)strlen(body));
    Rio_writen(fd, buf, strlen(buf));

    /* Build the HTTP response body */
    sprintf(body, "<html><title>Tiny Error</title>");
    sprintf(body, "%s<body bgcolor="
                  "ffffff"
                  ">\r\n",
            body);
    sprintf(body, "%s%s: %s\r\n", body, errnum, shortmsg);
    sprintf(body, "%s<p>%s: %s\r\n", body, longmsg, cause);
    sprintf(body, "%s<hr><em>The Sysstatd Web server</em>\r\n", body);
    Rio_writen(fd, body, strlen(body));
}

void read_requesthdrs(rio_t *rp) {
    char buf[MAXLINE];

    Rio_readlineb(rp, buf, MAXLINE);
    while (strcmp(buf, "\r\n")) {
        printf("%s", buf);
        ssize_t size = Rio_readlineb(rp, buf, MAXLINE);
        if (size < 0) {
            break;
        }
    }
    return;
}

// Parse uri into file name and cgiargs. For both static and dynamic
// If it is static, filename will be the path of that file in server, cgiargs will be ""
// If it is dynamic, filename will be the path of that cgi file in the server, cgiargs will be arguments of the excutable
// If it is /loadavg or /meminfo, filename will be "", cgiargs will be the callback function
// If it is /runloop, /alloanno or /freeannon, filename will be "", cigargs will be ""
int parse_uri(char *uri, char *filename, char *cgiargs) {
    // If it contains /loadavg
    if (strncmp(uri, "/loadavg", strlen("/loadavg")) == 0) {
        strcpy(filename, "");

        // /loadavgjunk  /loadavg/junk
        if (*(uri + 8) != '?' && *(uri + 8) != '\0') {
            return -1;
        }

        char *ptr = index(uri, '?');
        fflush(stdout);
        if (ptr) {        // if has arguments(?callback=)
            while (ptr) { // find the posistion of callback=
                ptr++;
                if (strncmp(ptr, "callback=", strlen("callback=")) == 0) {
                    ptr = index(ptr, '=');
                    ptr++;

                    int legal_callback = 0;
                    int i = 0;
                    while (*ptr && *ptr != '&') {
                        char c = *ptr;
                        if ((c >= 'a' && c <= 'z') || (c >= 'A' || c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.') {
                            cgiargs[i] = *ptr;
                            ptr++;
                            i++;
                        } else {
                            legal_callback = 1;
                        }
                    }
                    if (legal_callback == 0) {
                        cgiargs[i] = '\0';
                        return LOADAVG;
                    } else {
                        strcpy(cgiargs, "");
                        return LOADAVG;
                    }
                } else {
                    ptr = index(ptr, '&');
                    if (ptr == NULL) {
                        strcpy(cgiargs, "");
                        return LOADAVG;
                    }
                }
            }
        } else {
            strcpy(cgiargs, "");
            return LOADAVG;
        }
        return LOADAVG;
    } else if (strncmp(uri, "/meminfo", strlen("/meminfo")) == 0) {
        strcpy(filename, "");

        if (*(uri + 8) != '?' && *(uri + 8) != '\0') {
            return -1;
        }

        char *ptr = index(uri, '?');
        fflush(stdout);

        if (ptr) {        // if has arguments(?callback=)
            while (ptr) { // find the posistion of callback=
                ptr++;
                if (strncmp(ptr, "callback=", strlen("callback=")) == 0) {
                    ptr = index(ptr, '=');
                    ptr++;

                    int legal_callback = 0;
                    int i = 0;
                    while (*ptr && *ptr != '&') {
                        char c = *ptr;
                        if ((c >= 'a' && c <= 'z') || (c >= 'A' || c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.') {
                            cgiargs[i] = *ptr;
                            ptr++;
                            i++;
                        } else {
                            legal_callback = 1;
                        }
                    }
                    if (legal_callback == 0) {
                        cgiargs[i] = '\0';
                        return MEMINFO;
                    } else {
                        strcpy(cgiargs, "");
                        return MEMINFO;
                    }
                } else {
                    ptr = index(ptr, '&');
                    if (ptr == NULL) {
                        strcpy(cgiargs, "");
                        return MEMINFO;
                    }
                }
            }
        } else {
            strcpy(cgiargs, "");
            return MEMINFO;
        }
        return MEMINFO;
    } else if (strncmp(uri, "/runloop", strlen("/runloop")) == 0) {
        strcpy(filename, "");
        strcpy(cgiargs, "");
        return RUNLOOP;
    } else if (strncmp(uri, "/allocanon", strlen("/allocanon")) == 0) {
        strcpy(filename, "");
        strcpy(cgiargs, "");
        return ALLOCANON;
    } else if (strncmp(uri, "/freeanon", strlen("/freeanon")) == 0) {
        strcpy(filename, "");
        strcpy(cgiargs, "");
        return FREEANON;
    } else if (!strstr(uri, "cgi-bin")) {
        char path_buf[256];
        char path_buf1[256];
        strcpy(path_buf, uri + 6);
        strcpy(path_buf1, path);
        strcat(path_buf1, path_buf);
        strcpy(filename, path_buf1);

        if (uri[strlen(uri) - 1] == '/') {
            strcat(filename, "home.html");
        }

        strcpy(cgiargs, "");

        return STATIC;
    } else {
        strcpy(filename, ".");
        strcat(filename, uri);

        char *ptr;
        ptr = index(uri, '?');
        if (ptr) {
            strcpy(cgiargs, ptr + 1);
            *ptr = '\0';
        } else {
            strcpy(cgiargs, "");
        }

        return DYNAMIC;
    }
    return -1;
}

// serve_static : copy static file back to client
void serve_static(int fd, char *filename, int filesize) {
    int srcfd;
    char *srcp, filetype[MAXLINE], buf[MAXLINE];

    get_filetype(filename, filetype);
    sprintf(buf, "HTTP/1.0 200 OK\r\n");
    sprintf(buf, "%sServer: Sysstatd Web Server\r\n", buf);
    sprintf(buf, "%sContent-length: %d\r\n", buf, filesize);
    sprintf(buf, "%sContent-type: %s\r\n\r\n", buf, filetype);
    Rio_writen(fd, buf, strlen(buf));

    srcfd = open(filename, O_RDONLY, 0);
    srcp = mmap(0, filesize, PROT_READ, MAP_PRIVATE, srcfd, 0);
    close(srcfd);
    Rio_writen(fd, srcp, filesize);
    munmap(srcp, filesize);
}

// get_filetype : get filetype from file name. Used in Contente-type
void get_filetype(char *filename, char *filetype) {
    if (strstr(filename, ".html")) {
        strcpy(filetype, "text/html");
    } else if (strstr(filename, ".gif")) {
        strcpy(filetype, "image/gif");
    } else if (strstr(filename, ".jpg")) {
        strcpy(filetype, "image/jpeg");
    } else {
        strcpy(filetype, "text/plain");
    }
}

// serve_dynamic : run a CGI program and write back the output to fd which is the client
void serve_dynamic(int fd, char *filename, char *cgiargs) {
    char buf[MAXLINE], *emptylist[] = { NULL };

    sprintf(buf, "HTTP/1.0 200 OK\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Server: Sysstatd Web Server\r\n");
    Rio_writen(fd, buf, strlen(buf));

    if (fork() == 0) { /* child */
        /* Real server would set all CGI vars here */
        setenv("QUERY_STRING", cgiargs, 1);
        dup2(fd, STDOUT_FILENO);              /* Redirect stdout to client */
        execve(filename, emptylist, environ); /* Run CGI program */
    }

    if (wait(NULL) < 0) { /* Parent waits for and reaps child */
        fprintf(stderr, "Wait Error.\n");
        exit(1);
    }
}

void send_response(int fd, char *msg, char *content_type, char *version) {
    char msg_buf[MAXLINE];
    char header_buf[MAXBUF];

    sprintf(msg_buf, "%s", msg);

    sprintf(header_buf, "%s 200 OK\r\n", version);
    Rio_writen(fd, header_buf, strlen(header_buf));

    sprintf(header_buf, "Content-Type: %s\r\n", content_type);
    Rio_writen(fd, header_buf, strlen(header_buf));

    sprintf(header_buf, "Content-Length: %d\r\n", (int)strlen(msg_buf));
    Rio_writen(fd, header_buf, strlen(header_buf));

    if (strncmp(version, "HTTP/1.0", strlen("HTTP/1.0")) == 0) {
        sprintf(header_buf, "Connection: close\r\n");
        Rio_writen(fd, header_buf, strlen(header_buf));
    }

    sprintf(header_buf, "%s\r\n", "");
    Rio_writen(fd, header_buf, strlen(header_buf));

    Rio_writen(fd, msg_buf, strlen(msg_buf));
}

static void run_loop(void) {
    time_t begin = time(NULL);

    while ((time(NULL) - begin) < 15) {
        continue;
    }
    return;
}

/********************************
 * Client/server helper functions
 ********************************/
/* This is from CSAPP course website
 * open_listenfd - Open and return a listening socket on port. This
 *     function is reentrant and protocol-independent.
 *
 *     On error, returns:
 *       -2 for getaddrinfo error
 *       -1 with errno set for other errors.
 */
/* $begin open_listenfd */
int open_listenfd(char *port) {
    struct addrinfo hints, *listp, *p;
    int listenfd, rc, optval = 1;

    /* Get a list of potential server addresses */
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_flags = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;             /* Accept connections */
    hints.ai_flags = AI_PASSIVE | AI_ADDRCONFIG; /* ... on any IP address */
    hints.ai_flags |= AI_NUMERICSERV;            /* ... using port number */
    if ((rc = getaddrinfo(NULL, port, &hints, &listp)) != 0) {
        fprintf(stderr, "getaddrinfo failed (port %s): %s\n", port, gai_strerror(rc));
        return -2;
    }

    /* Walk the list for one that we can bind to */
    for (p = listp; p; p = p->ai_next) {
        if (p->ai_family == AF_INET) {
            continue;
        }
        /* Create a socket descriptor */
        if ((listenfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) < 0)
            continue; /* Socket failed, try the next */

        /* Eliminates "Address already in use" error from bind */
        setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, // line:netp:csapp:setsockopt
                   (const void *)&optval, sizeof(int));

        /* Bind the descriptor to the address */
        if (bind(listenfd, p->ai_addr, p->ai_addrlen) == 0)
            break;                 /* Success */
        if (close(listenfd) < 0) { /* Bind failed, try the next */
            fprintf(stderr, "open_listenfd close failed: %s\n", strerror(errno));
            return -1;
        }
    }

    /* Clean up */
    freeaddrinfo(listp);
    if (!p) /* No address worked */
        return -1;

    /* Make it a listening socket ready to accept connection requests */
    if (listen(listenfd, LISTENQ) < 0) {
        close(listenfd);
        return -1;
    }
    return listenfd;
}
/* $end open_listenfd */

/****************************************************
 * Wrappers for reentrant protocol-independent helpers
 ****************************************************/
int Open_listenfd(char *port) {
    int rc;
    if ((rc = open_listenfd(port)) < 0)
        unix_error("Open_listenfd error");
    return rc;
}
