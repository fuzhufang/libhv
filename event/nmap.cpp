#include "nmap.h"
#include "hloop.h"
#include "netinet.h"
#include "hstring.h"

#define MAX_RECVFROM_TIMEOUT    5000 // ms

typedef struct recvfrom_udata_s {
    Nmap*   nmap;
    int     send_cnt;
    int     recv_cnt;
    int     up_cnt;
} recvfrom_udata_t;

static void on_idle(hidle_t* idle) {
    hloop_stop(idle->loop);
}

static void on_timer(htimer_t* timer) {
    hloop_stop(timer->loop);
}

static void on_recvfrom(hio_t* io, void* buf, int readbytes) {
    //printf("on_recv fd=%d readbytes=%d\n", io->fd, readbytes);
    //char localaddrstr[INET6_ADDRSTRLEN+16] = {0};
    //char peeraddrstr[INET6_ADDRSTRLEN+16] = {0};
    //printf("[%s] <=> [%s]\n",
            //sockaddr_snprintf(io->localaddr, localaddrstr, sizeof(localaddrstr)),
            //sockaddr_snprintf(io->peeraddr, peeraddrstr, sizeof(peeraddrstr)));
    recvfrom_udata_t* udata = (recvfrom_udata_t*)io->userdata;
    if (++udata->recv_cnt == udata->send_cnt) {
        hloop_stop(io->loop);
    }
    Nmap* nmap = udata->nmap;
    struct sockaddr_in* peeraddr = (struct sockaddr_in*)io->peeraddr;
    auto iter = nmap->find(peeraddr->sin_addr.s_addr);
    if (iter != nmap->end()) {
        if (iter->second == 0) {
            iter->second = 1;
            if (++udata->up_cnt == nmap->size()) {
                hloop_stop(io->loop);
            }
        }
        else {
            //hloop_stop(io->loop);
        }
    }
    //else {
        //hloop_stop(io->loop);
    //}
}

int nmap_discovery(Nmap* nmap) {
    // socket
    int sockfd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sockfd < 0) {
        perror("socket");
        if (errno == EPERM) {
            fprintf(stderr, "please use root or sudo to create a raw socket.\n");
        }
        return -socket_errno();
    }

    hloop_t loop;
    hloop_init(&loop);
    uint64_t start_hrtime = hloop_now_hrtime(&loop);

    nonblocking(sockfd);
    hio_t* io = hio_get(&loop, sockfd);
    if (io == NULL) return -1;
    io->io_type = HIO_TYPE_IP;
    struct sockaddr_in localaddr;
    socklen_t addrlen = sizeof(localaddr);
    memset(&localaddr, 0, addrlen);
    localaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    hio_setlocaladdr(io, (struct sockaddr*)&localaddr, addrlen);

    recvfrom_udata_t udata;
    udata.nmap = nmap;
    udata.send_cnt = 0;
    udata.recv_cnt = 0;
    udata.up_cnt = 0;
    io->userdata = &udata;
    char recvbuf[128];
    hrecvfrom(&loop, sockfd, recvbuf, sizeof(recvbuf), on_recvfrom);
    // icmp
    char sendbuf[64];
    icmp_t* icmp_req = (icmp_t*)sendbuf;
    icmp_req->icmp_type = ICMP_ECHO;
    icmp_req->icmp_code = 0;
    icmp_req->icmp_id = getpid();
    for (int i = 0; i < sizeof(sendbuf) - sizeof(icmphdr_t); ++i) {
        icmp_req->icmp_data[i] = i;
    }
    auto iter = nmap->begin();
    struct sockaddr_in peeraddr;
    while (iter != nmap->end()) {
        icmp_req->icmp_seq = iter->first;
        icmp_req->icmp_cksum = 0;
        icmp_req->icmp_cksum = checksum((uint8_t*)icmp_req, sizeof(sendbuf));
        socklen_t addrlen = sizeof(peeraddr);
        memset(&peeraddr, 0, addrlen);
        peeraddr.sin_family = AF_INET;
        peeraddr.sin_addr.s_addr = iter->first;
        hio_setpeeraddr(io, (struct sockaddr*)&peeraddr, addrlen);
        hsendto(&loop, sockfd, sendbuf, sizeof(sendbuf), NULL);
        ++udata.send_cnt;
        ++iter;
    }

    htimer_add(&loop, on_timer, MAX_RECVFROM_TIMEOUT, 1);
    hidle_add(&loop, on_idle, 1);

    hloop_run(&loop);
    uint64_t end_hrtime = hloop_now_hrtime(&loop);

    // print result
    char ip[INET_ADDRSTRLEN];
    iter = nmap->begin();
    while (iter != nmap->end()) {
        inet_ntop(AF_INET, (void*)&iter->first, ip, sizeof(ip));
        printd("%s\t is %s.\n", ip, iter->second == 0 ? "down" : "up");
        ++iter;
    }
    printd("Nmap done: %lu IP addresses (%d hosts up) scanned in %.2f seconds\n",
            nmap->size(), udata.up_cnt, (end_hrtime-start_hrtime)/1000000.0f);

    return udata.up_cnt;
}

int segment_discovery(const char* segment16, Nmap* nmap) {
    StringList strlist = split(segment16, '.');
    if (strlist.size() != 4) return -1;
    uint32_t addr = 0;
    uint8_t* p = (uint8_t*)&addr;
    p[0] = atoi(strlist[0].c_str());
    p[1] = atoi(strlist[1].c_str());
    p[3] = 1;
    printd("Nmap scan %u.%u.x.1...\n", p[0], p[1]);
    nmap->clear();
    for (int i = 0; i < 256; ++i) {
        p[2] = i;
        (*nmap)[addr] = 0;
    }
    return nmap_discovery(nmap);
}

int host_discovery(const char* segment24, Nmap* nmap) {
    StringList strlist = split(segment24, '.');
    if (strlist.size() != 4) return -1;
    uint32_t addr = 0;
    uint8_t* p = (uint8_t*)&addr;
    p[0] = atoi(strlist[0].c_str());
    p[1] = atoi(strlist[1].c_str());
    p[2] = atoi(strlist[2].c_str());
    printd("Nmap scan %u.%u.%u.x...\n", p[0], p[1], p[2]);
    // 0,255 reserved
    nmap->clear();
    for (int i = 1; i < 255; ++i) {
        p[3] = i;
        (*nmap)[addr] = 0;
    }
    return nmap_discovery(nmap);
}
