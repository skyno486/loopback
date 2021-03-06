#include <cstring>
#include "main.h"

int main(int argc, void** argv)
{
    GREEN("VERSION:%s\n\n", VERSION);

    int mode = 0;
    std::thread *th = NULL;
    auto networks = GetNetworkInterface();
    YELLOW("Network Interfaces\n");
    for(auto it: networks){
        printf("[%s] %s\n", it.interface, it.address);
    }
    std::map<int, std::string> args;
    make_map();
    int rs = parse_arg(argc,argv, args);
    switch (rs){
        case e_ARG_VERSION:
        case e_ARG_HELP:
        case e_ARG_EXCEPT:
            RED("ERROR Param\n");
            return 0;
        default:
            break;
    }

    if(args.find(e_ARG_INPUT_PCAP) != args.end()){
        mode = e_MODE_FILE_TO_UDP;
    } else if(args.find(e_ARG_INPUT_NIC) != args.end() &&
              args.find(e_ARG_OUTPUT_NIC) != args.end()){
       mode = e_MODE_BYPASS;
    } else if(args.find(e_ARG_INPUT_UDP) != args.end() &&
              args.find(e_ARG_OUTPUT_UDP) != args.end()){
        mode = e_MODE_UDP_TO_UDP;
    }
    printf("\n\n");

    switch (mode) {
        case e_MODE_BYPASS:
            YELLOW("[Mode] Bypass Mode\n");
            th = new std::thread(thread_loop, args, networks);
            break;
        case e_MODE_FILE_TO_UDP:
            YELLOW("[Mode] Pcap Mode\n");
            th = new std::thread(thread_pcap, args, networks);
            break;
        case e_MODE_UDP_TO_UDP:
            YELLOW("[Mode] UDP to UDP\n");
            th = new std::thread(thread_sender, args, networks);
            break;
        default:
            break;
    }
    while(true){
        sleep(1);
    }
    return 0;
}


void thread_pcap(std::map<int, std::string> arg, std::list<Ethernet> networks)
{
    PcapHeader mainhdr;
    PcapPacketHeader pkhdr;
    double first_time = 0;
    double pcap_curr_time = 0;
    timespec timespec_check;
    timespec timespec_1sec;
    long long file_length;
    long long file_pos = 0;
    unsigned char ip_buffer[10240] = { 0, };
    ip_header_t ip_h;
    udp_header_t udp_h;
    bool use_output_nic = false;
    bool use_re_stamp = false;
    std::string path;
    sockaddr_in server_addr;
    int send_bitrate = 0;
    u_int sock= 0;
    u_int addr_len = sizeof(struct sockaddr);
    FILE *file;
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    memset(&server_addr, 0, addr_len);
    in_addr localInterface;
    auto pcap_path = arg.find(e_ARG_INPUT_PCAP);
    auto output_nic = arg.find(e_ARG_OUTPUT_NIC);
    auto output_udp = arg.find(e_ARG_OUTPUT_UDP);
    InOutParam output_param;
    setlocale(LC_NUMERIC, "");

    if(output_nic != arg.end()){
        for(auto it: networks){
            std::string interface = it.interface;
            if(output_nic->second.compare("lo") == 0){
                localInterface.s_addr = inet_addr(it.address);
                if (setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, (char *)&localInterface, sizeof(localInterface)) < 0) {
                    printf("setting local interface\n");
                }
                use_output_nic = true;
                printf("OutputNIC[lo], 127.0.0.1\n");
                break;
            }
            if(output_nic->second.compare(interface) == 0){
                localInterface.s_addr = inet_addr(it.address);
                if (setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, (char *)&localInterface, sizeof(localInterface)) < 0) {
                    printf("setting local interface\n");
                }
                use_output_nic = true;
                printf("OutputNIC[%s], %s\n", it.interface, it.address);
                break;
            }
        }
    }
    if(output_udp != arg.end()){
        use_re_stamp = true;
        output_param = parse_inout(output_udp->second);
        auto addrs = split(output_param.udp, (char*)":");
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(std::stoi(addrs[1]));
        server_addr.sin_addr.s_addr = inet_addr(addrs[0].c_str());
        localInterface.s_addr = inet_addr(output_param.adapter.c_str());
        if (setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, (char *)&localInterface, sizeof(localInterface)) < 0) {
            printf("setting local interface\n");
        }
    }
    if(use_output_nic == false && use_re_stamp){
        return;
    }

    if(pcap_path != arg.end()){
       path = pcap_path->second;
    }

    file = fopen(path.c_str(), "rb");
    if (file == NULL) {
        RED("ERROR PCAP Open : %s\n", path.c_str());
    } else {
        YELLOW("StartPcap : %s\n", path.c_str());
        usleep(1000000);
        fseek(file, 0, SEEK_END);
        file_length = ftell(file);
        fseek(file, 0, SEEK_SET);
        clock_gettime(CLOCK_MONOTONIC, &timespec_1sec);
        while (gRunning) {
            file_pos = 0;
            int read = fread(&mainhdr, 1, sizeof(mainhdr), file);
            first_time = 0;
            send_bitrate = 0;
            if (read > 0) {
                file_pos += read;
                while (file_length > file_pos && gRunning == true) {
                    read = fread(&pkhdr, 1, sizeof(pkhdr), file);
                    if (read > 0) {
                        file_pos += read;
                        if (first_time == 0) {
                            first_time = pkhdr.ts_sec + (pkhdr.ts_usec / 1000000.0);
                            clock_gettime(CLOCK_MONOTONIC, &timespec_check);
                        }
                        pcap_curr_time = pkhdr.ts_sec + (pkhdr.ts_usec / 1000000.0);
                        double time = pcap_curr_time - first_time;
                        while (gRunning) {
                            if (diff_time(timespec_check) >= time) {
                                break;
                            }
                            usleep(100);
                        }
                        read = fread(ip_buffer, 1, pkhdr.incl_len, file);
                        if (read > 0) {
                            file_pos += read;
                            if(use_output_nic){
                                parse_ip(ip_buffer + 14, &ip_h);
                                parse_udp(ip_buffer + 14 + 20, &udp_h);
                                server_addr.sin_family = AF_INET;
                                server_addr.sin_port = htons(udp_h.dest_port);
                                server_addr.sin_addr.s_addr = htonl(ip_h.dest);
                            }
                            if (sendto(sock, ip_buffer + 14 + 28, read - 14 - 28, 0,
                                       (struct sockaddr *) &server_addr, sizeof(server_addr)) < 0) {
                            }else{
                                send_bitrate += read - 28;
                            }
                            usleep(100);
                        }
                    }
                    if (diff_time(timespec_1sec) >= 1.0) {
                        clock_gettime(CLOCK_MONOTONIC, &timespec_1sec);
                        printf("Sending : %'d bytes\n", send_bitrate);
                        send_bitrate = 0;
                    }
                }
            }
            fseek(file, 0, SEEK_SET);
            YELLOW("Loop %s\n", path.c_str());
            usleep(1000);
        }
        fclose(file);
    }
}

void thread_loop(std::map<int, std::string> arg, std::list<Ethernet> networks){
    char err[1024] = {0,};
    pcap_t* i_hdl;
    pcap_t* o_hdl;
    pcap_pkthdr pcap_pkt;
    i_hdl = pcap_open_live(arg[e_ARG_INPUT_NIC].c_str(), 2000, 1, 10, err);
    o_hdl = pcap_open_live(arg[e_ARG_OUTPUT_NIC].c_str(), 2000, 1, 10, err);
    while(gRunning){
        const uint8_t *packet = pcap_next(i_hdl, &pcap_pkt);
        pcap_sendpacket(o_hdl, packet, pcap_pkt.len);
        usleep(1);
    }
}

void thread_sender(std::map<int, std::string> arg, std::list<Ethernet> networks)
{
    char err[1024] = {0,};
    ip_header_t ip_h;
    udp_header_t  udp_h;
    sockaddr_in server_addr;
    struct ip_mreq group;
    u_int sender= 0;
    u_int receiver = 0;
    u_int addr_len = sizeof(struct sockaddr);
    pcap_t* i_hdl = NULL;
    pcap_t* o_hdl = NULL;
    pcap_pkthdr pcap_pkt;
    receiver = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sender = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);

    auto input_udp = arg.find(e_ARG_INPUT_UDP);
    auto output_udp = arg.find(e_ARG_OUTPUT_UDP);
    InOutParam output_param;
    InOutParam input_param;
    struct in_addr localInterface;
    input_param = parse_inout(input_udp->second);
    auto addrs = split(input_param.udp, (char *) ":");
    int recv_port = 0;

    server_addr.sin_family = PF_INET;
    server_addr.sin_addr.s_addr = inet_addr(input_param.adapter.c_str());

    if(bind(receiver, (struct  sockaddr*)&server_addr, addr_len) < 0){
        printf("error bind\n");
    }
    group.imr_multiaddr.s_addr = inet_addr(addrs[0].c_str());
    group.imr_interface.s_addr = inet_addr(input_param.adapter.c_str());

    if (setsockopt(receiver, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char *)&group, sizeof(group)) < 0) {
        printf("err\n");
    }else{
        printf("\n[Input UDP]\n");
        printf("UDP : %s\n", addrs[0].c_str());
        printf("Interface IP : %s\n", input_param.adapter.c_str());
        recv_port = std::stoi(addrs[1]);
    }
    if(output_udp != arg.end()) {
        output_param = parse_inout(output_udp->second);
        auto addrs = split(output_param.udp, (char *) ":");
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(std::stoi(addrs[1]));
        server_addr.sin_addr.s_addr = inet_addr(addrs[0].c_str());
        localInterface.s_addr = inet_addr(output_param.adapter.c_str());
        if (setsockopt(sender, IPPROTO_IP, IP_MULTICAST_IF, (char *) &localInterface, sizeof(localInterface)) < 0) {
            printf("setting local interface\n");
        }
        printf("\n[Output UDP]\n");
        printf("UDP : %s:%d\n", addrs[0].c_str(), std::stoi(addrs[1]));
        printf("Interface IP : %s\n", output_param.adapter.c_str());
    }

    for(auto it: networks){
        std::string ip = it.address;
        if(ip.compare(input_param.adapter) == 0){
            i_hdl = pcap_open_live(it.interface, 2000, 1, 10, err);
            break;
        }
    }

    if(i_hdl == NULL){
        printf("Input Error\n");
       return;
    }


    timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    int received_bytes = 0;
    int send_bytes = 0;
    int error_count = 0;
    setlocale(LC_NUMERIC, "");
    while(gRunning){
        const uint8_t *packet = pcap_next(i_hdl, &pcap_pkt);
        if(packet != NULL){
            parse_ip(packet + 14, &ip_h);
            parse_udp(packet + 14 + 20, &udp_h);
            if(udp_h.dest_port == recv_port){
                received_bytes += udp_h.length - 8;
                if(sendto(sender, packet + 28 + 14, pcap_pkt.len - 28 - 14, 0,
                          (struct sockaddr*) &server_addr, sizeof(server_addr)) < 0 ){
                    error_count++;
                }else{
                    send_bytes += pcap_pkt.len - 28 - 14;
                }
            }
        }
        if(diff_time(time) >= 1.0){
            printf("[R] %'d bytes > [S] %'d bytes\n", received_bytes, send_bytes);
            if(error_count> 0){
                RED("SendError : %d\n", error_count);
            }
            received_bytes = 0;
            send_bytes = 0;
            error_count = 0;
            clock_gettime(CLOCK_MONOTONIC, &time);
        }
        usleep(1);
    }
}
