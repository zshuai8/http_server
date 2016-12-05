Protocol:
    Naming scheme
    Delivery mechanism: encoding bits, packaging bits into frames

Packet:
    header: packet size, source address and destination address
    payload


The set of hosts is mapped to a set of 32-bit IP address
The set of IP address is mapped to a set of identifiers called domain names

struct in_addr{
    unsigned int s_addr;
};

Unix provides the following functions for converting
between network and host byte order:
#include<netinet/in.h>
unsigned long int htonl(unsigned long int hostlong);
unsinged short int htons(unsigned short int hostshort);

unsigned long int ntohl(unsigned long int netlong);
unsigned short int ntohs(unsigned short int netshort);


Internet programs convert back and forth between IP addresses
and dotted- decimal strings using the functions inet_aton and inet_ntoa:
#include <arpa/inet.h>
int inet_aton(const char *cp, struct in_addr *inp);
char *inet_ntoa(struct in_arrd in);

/* DNS host entry structure */
struct hostent {
   char   *h_name;
   char   **h_aliases;
   int    h_addrtype;
   int    h_length;
   char   **h_addr_list;
}

#include <netdb.h>
struct hostent *gethostbyname(const char *name);
    Returns: non-NULL pointer if OK, NULL pointer on error with h_errno set
struct hostent *gethostbyaddr(const char *addr, int len, 0);
    Returns: non-NULL pointer if OK, NULL pointer on error with h_errno set

socket: socket address: internet address and 16 bit port
    address:port

web servers use port 80
email servers use port 25
/etc/services contains list of services and port


socket pair (cliaddr:cliport, servaddr:servport)

16 byte sockaddr_in

/* Generic socket address structure (for connect, bind, and accept) */
struct sockaddr {
    unsigned short  sa_family;   /* Protocol family */
    char            sa_data[14]; /* Address data.  */
};

/* Internet-style socket address structure */
struct sockaddr_in  {
    unsigned short  sin_family;  /* Address family (always AF_INET) */
    unsigned short  sin_port;    /* Port number in network byte order */
    struct in_addr  sin_addr;    /* IP address in network byte order */
    unsigned char   sin_zero[8]; /* Pad to sizeof(struct sockaddr) */
};


#include<sys/types.h>
#include<sys/socket.h>
int socket(int domain,int type,int protocol);

clientfd = socket(AF_INET, SOCK_STREAM, 0);
AF_INET: internet
SOCK_STREAM: end point for an internet connection

#inlcude<sys/socket.h>
int connect(int sockfd,struct sockaddr *serv_addr, int addrlen);


int open_clientfd(char *hostname, int port){
    int clientfd;
    struct hostent *hp;
    struct sockaddr_in serveraddr;

    if ((clientfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        return -1; /* Check errno for cause of error */

    /* Fill in the server’s IP address and port */
    if ((hp = gethostbyname(hostname)) == NULL)
        return -2; /* Check h_errno for cause of error */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    bcopy((char *)hp->h_addr_list[0],
          (char *)&serveraddr.sin_addr.s_addr, hp->h_length);
    serveraddr.sin_port = htons(port);

    /* Establish a connection with the server */
    if (connect(clientfd, (SA *) &serveraddr, sizeof(serveraddr)) < 0)
        return -1;
    return clientfd;
}


server: bind, listen, accept
#include<sys/socket.h>
int bind(int sockfd, struct sockaddr *my_addr, int addrlen);

#include<sys/socket.h>
int listen(int sockfd, int backlog);


int open_listenfd(int port){
    in listenfd,optval=1;
    struct sockaddr_in serveraddr;

    if(listenfd=socket(AF_INET,SOCK_STREAM,0)<0)
        return -1;

    if(setsockopt(listenfd,SOL_SOCKET,SO_REUSEADDR,
                (const void *)&optval, sizeof(int)) < 0)
        return -1;

    bzero((char *)&serveraddr , sizeof(serveraddr));
    serveraddr.sin_family=AF_INET;
    serveraddr.sin_addr.s_addr=htonl(INADDR_ANY);
    serveraddr.sin_port=htons((unsigned short)port);

    if (bind(listenfd, (SA *)&serveraddr, sizeof(serveraddr)) < 0)
        return -1;

    /* Make it a listening socket ready to accept connection requests */
    if (listen(listenfd, LISTENQ) < 0)
        return -1;

    return listenfd;
}

#include<sys/socket.h>
int accept(int listenfd, struct sockaddr *addr, int *addrlen);

HTTP(Hypertext Transfer Protocol)
port 80

? separate excutable and arguments
& separate arguments

http://bluefish.ics.cs.cmu.edu:8000/cgi-bin/adder?15000&213

HTTP request:
    a request line: <method> <uri> <version>
        method: GET, POST, OPTIONS, HEAD, PUT, DELETE, TRACE
        uri: uniform resource identifier. suffix of the url
        version: HTTP/1.1

    follwed by zore or more request headers
        <header name>:<header data>
        Host:www.aol.com

    followed by an empty text line that terminates the list of headers

HTTP response:
    a response line: <version> <status code> <status message>
        version: http version
        status code: three-digit positive integer

    zero or more response header
        Content-Type
        Content-Length

    empty line that terminates the headers

    response body

CGI(Common Gateway Interface)
    GET: in url. ? separate filename and arguments. each argument is separated by &. space= "%20"
    POST: arguments in request body

GET /cgi-bin/adder?15000&213 HTTP/1.1
    fork child and call execve
    set QUERY_STRING to "15000&213". getenv

CGI program output to stdout
redirect to connect descriptor
Child is responsible for generating the Content-Type and Content-Length


persistent connection
/loadavg/junk
/meminfo/junk

/loadavgjunk
/meminfojunk