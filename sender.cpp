/* BASELINE SENDER (C) — naive on purpose. Rewrite it (C, C++, Go, or Rust).
 *
 * Ports (all 127.0.0.1):
 *   bind 47010  <- harness source delivers frame i here at t0 + i*20ms
 *                  (format: 4-byte big-endian seq + 160-byte payload)
 *   send 47001  -> relay uplink toward the receiver (YOUR wire format)
 *   bind 47004  <- feedback from your receiver, via the relay (optional)
 *
 * This baseline forwards each frame once, unchanged, and ignores feedback.
 * No redundancy, no retransmission. It cannot pass. That is the point.
 *
 * Env vars available if you want them: T0 (epoch seconds, float),
 * DURATION_S, DELAY_MS. The harness kills this process when the run ends,
 * so a forever-loop is fine.
 *
 * build: make        run: python3 run.py --delay_ms 60
 */
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>
#include <stdint.h>
#include <unordered_map>
#include <vector>

int main(void) {
    // 1. Setup Incoming Port (from Harness)
    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in in_addr = {0};
    in_addr.sin_family = AF_INET;
    in_addr.sin_port = htons(47010);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    bind(in_fd, (struct sockaddr *)&in_addr, sizeof(in_addr));

    // 2. Setup Outgoing Port (to Relay)
    int out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in relay = {0};
    relay.sin_family = AF_INET;
    relay.sin_port = htons(47001);
    relay.sin_addr.s_addr = inet_addr("127.0.0.1");

    // 3. Setup Feedback Port (from Relay)
    int feedback_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in fb_addr = {0};
    fb_addr.sin_family = AF_INET;
    fb_addr.sin_port = htons(47004);
    fb_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    bind(feedback_fd, (struct sockaddr *)&fb_addr, sizeof(fb_addr));

    std::unordered_map<uint32_t, std::vector<uint8_t>> history;
    unsigned char buf[164];
    unsigned char prev_payload[160];

    int max_fd = (in_fd > feedback_fd) ? in_fd : feedback_fd;

    for (;;) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(in_fd, &readfds);
        FD_SET(feedback_fd, &readfds);

        // Block until either a new frame or a NACK arrives
        select(max_fd + 1, &readfds, NULL, NULL, NULL);

        // -- HANDLE NACKS (ARQ) --
        if (FD_ISSET(feedback_fd, &readfds)) {
            uint32_t nack_net;
            if (recvfrom(feedback_fd, &nack_net, 4, 0, NULL, NULL) > 0) {
                uint32_t missing_seq = ntohl(nack_net);
                if (history.count(missing_seq)) {
                    sendto(out_fd, history[missing_seq].data(), 164, 0, 
                           (struct sockaddr *)&relay, sizeof(relay));
                }
            }
        }

        // -- HANDLE NEW FRAMES (FEC) --
        if (FD_ISSET(in_fd, &readfds)) {
            ssize_t n = recvfrom(in_fd, buf, sizeof(buf), 0, NULL, NULL);
            if (n > 0) {
                sendto(out_fd, buf, n, 0, (struct sockaddr *)&relay, sizeof(relay));

                uint32_t seq;
                memcpy(&seq, buf, 4);
                uint32_t host_seq = ntohl(seq);
                
                // Store in history for potential NACKs
                history[host_seq] = std::vector<uint8_t>(buf, buf + n);

                if (host_seq % 2 == 0) {
                    memcpy(prev_payload, buf + 4, 160);
                } else {
                    unsigned char fec_buf[164];
                    uint32_t fec_seq = htonl(host_seq | 0x80000000); // Flag MSB
                    memcpy(fec_buf, &fec_seq, 4);
                    
                    uint64_t* p_prev = (uint64_t*)prev_payload;
                    uint64_t* p_curr = (uint64_t*)(buf + 4);
                    uint64_t* p_fec = (uint64_t*)(fec_buf + 4);
                    
                    for (int i = 0; i < 20; ++i) p_fec[i] = p_prev[i] ^ p_curr[i];
                    
                    sendto(out_fd, fec_buf, 164, 0, (struct sockaddr *)&relay, sizeof(relay));
                }
            }
        }
    }
    return 0;
}