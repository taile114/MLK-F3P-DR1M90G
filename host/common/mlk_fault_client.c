#define _POSIX_C_SOURCE 200809L
#include "mlk_fault_client.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

static int64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC,&t);
    return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}
static int wait_fd(int fd, short events, int64_t deadline) {
    for (;;) {
        int64_t left=deadline-now_ms();
        if (left<=0) { errno=ETIMEDOUT; return -1; }
        struct pollfd p={fd,events,0};
        int n=poll(&p,1,(int)left);
        if (n>0) return 0;
        if (n==0) { errno=ETIMEDOUT; return -1; }
        if (errno!=EINTR) return -1;
    }
}
/* API v1 与 v2 共用 connect/send/recv；两者只差请求正文上限与堆发送缓冲大小。 */
static int publish(const char *path, const char *request, size_t limit,
                   char *reply, size_t capacity, int timeout_ms) {
    struct sockaddr_un addr;
    const char *selected=path ? path : "/run/mlk-fault.sock";
    size_t size=0, sent=0, received=0;
    char *data=NULL;
    int fd=-1, result=-1;
    int64_t deadline=0;
    if (!request || !reply || capacity<2 || timeout_ms<1 || timeout_ms>10000
        || strlen(selected)>=sizeof(addr.sun_path)) { errno=EINVAL; return -1; }
    size=strlen(request);
    if (size>limit || strchr(request,'\n')) { errno=EINVAL; return -1; }
    deadline=now_ms()+timeout_ms;
    fd=socket(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
    if (fd<0) return -1;
    memset(&addr,0,sizeof(addr)); addr.sun_family=AF_UNIX;
    strcpy(addr.sun_path,selected);
    if (connect(fd,(struct sockaddr *)&addr,sizeof(addr))<0) {
        int error=0; socklen_t len=sizeof(error);
        if (errno!=EINPROGRESS || wait_fd(fd,POLLOUT,deadline)<0) goto done;
        if (getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&len)<0) goto done;
        if (error) { errno=error; goto done; }
    }
    data=malloc(limit+2);
    if (!data) { errno=ENOMEM; goto done; }
    memcpy(data,request,size); data[size++]='\n';
    while (sent<size) {
        if (wait_fd(fd,POLLOUT,deadline)<0) goto done;
        ssize_t n=send(fd,data+sent,size-sent,MSG_NOSIGNAL);
        if (n>0) sent+=(size_t)n;
        else if (n==0) { errno=EPIPE; goto done; }
        else if (errno!=EAGAIN && errno!=EINTR) goto done;
    }
    while (received+1<capacity) {
        if (wait_fd(fd,POLLIN,deadline)<0) goto done;
        ssize_t n=recv(fd,reply+received,capacity-1-received,0);
        if (n>0) {
            received+=(size_t)n; reply[received]=0;
            if (strchr(reply,'\n')) { result=0; goto done; }
        } else if (!n) { errno=ECONNRESET; goto done; }
        else if (errno!=EAGAIN && errno!=EINTR) goto done;
    }
    errno=EMSGSIZE;
done: {
        int saved=errno; free(data); close(fd); errno=saved;
        return result;
    }
}
int mlk_fault_publish_json(const char *path, const char *request,
                          char *reply, size_t capacity, int timeout_ms) {
    return publish(path,request,999,reply,capacity,timeout_ms);
}
int mlk_fault_publish_json_v2(const char *path, const char *request,
                             char *reply, size_t capacity, int timeout_ms) {
    return publish(path,request,4095,reply,capacity,timeout_ms);
}
