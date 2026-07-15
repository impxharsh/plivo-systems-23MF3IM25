/* BASELINE RECEIVER (C) — naive on purpose. Rewrite it (C, C++, Go, or Rust).
 *
 * Ports (all 127.0.0.1):
 *   bind 47002  <- media from your sender, via the hostile relay
 *   send 47020  -> harness player. MUST be: 4-byte big-endian seq +
 *                  160-byte payload. Frame i counts only if it arrives
 *                  BEFORE its deadline t0 + DELAY_MS + i*20ms.
 *   send 47003  -> feedback to your sender, via the relay (optional)
 *
 * This baseline forwards whatever arrives straight to the player: lost
 * frames stay lost, late frames stay late, duplicates are re-sent
 * harmlessly. All yours to fix — jitter buffer, reordering, recovery.
 *
 * Env vars available: T0, DURATION_S, DELAY_MS. Harness kills the process
 * at run end; a forever-loop is fine.
 */
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdint.h>
#include <unordered_map>
#include <vector>

std::unordered_map<uint32_t, std::vector<uint8_t>> jb;
std::unordered_map<uint32_t, std::vector<uint8_t>> fec_map;
bool forwarded[65536] = {false};
uint32_t max_seen = 0;

int out_fd;
struct sockaddr_in player;

// Forward exactly once to prevent double-counting
void forward_packet(uint32_t seq, const std::vector<uint8_t>& pkt) {
    if (!forwarded[seq]) {
        sendto(out_fd, pkt.data(), pkt.size(), 0, (struct sockaddr *)&player, sizeof(player));
        forwarded[seq] = true;
    }
}

// Attempt XOR Recovery
void recover_if_possible(uint32_t odd_seq) {
    uint32_t even_seq = odd_seq - 1;
    if (fec_map.count(odd_seq)) {
        if (jb.count(even_seq) && !jb.count(odd_seq)) {
            std::vector<uint8_t> rec(164);
            uint32_t net_seq = htonl(odd_seq);
            memcpy(rec.data(), &net_seq, 4);
            
            uint64_t* p_even = (uint64_t*)(jb[even_seq].data() + 4);
            uint64_t* p_fec = (uint64_t*)(fec_map[odd_seq].data() + 4);
            uint64_t* p_rec = (uint64_t*)(rec.data() + 4);
            for (int i = 0; i < 20; ++i) p_rec[i] = p_even[i] ^ p_fec[i];
            
            jb[odd_seq] = rec;
            forward_packet(odd_seq, rec);
        } 
        else if (!jb.count(even_seq) && jb.count(odd_seq)) {
            std::vector<uint8_t> rec(164);
            uint32_t net_seq = htonl(even_seq);
            memcpy(rec.data(), &net_seq, 4);
            
            uint64_t* p_odd = (uint64_t*)(jb[odd_seq].data() + 4);
            uint64_t* p_fec = (uint64_t*)(fec_map[odd_seq].data() + 4);
            uint64_t* p_rec = (uint64_t*)(rec.data() + 4);
            for (int i = 0; i < 20; ++i) p_rec[i] = p_odd[i] ^ p_fec[i];
            
            jb[even_seq] = rec;
            forward_packet(even_seq, rec);
        }
    }
}

int main(void) {
    // 1. Setup Incoming Port (from Relay)
    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in in_addr = {0};
    in_addr.sin_family = AF_INET;
    in_addr.sin_port = htons(47002);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    bind(in_fd, (struct sockaddr *)&in_addr, sizeof(in_addr));

    // 2. Setup Player Port
    out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    player.sin_family = AF_INET;
    player.sin_port = htons(47020);
    player.sin_addr.s_addr = inet_addr("127.0.0.1");

    // 3. Setup Feedback Port (to Relay)
    int feedback_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in relay_feedback = {0};
    relay_feedback.sin_family = AF_INET;
    relay_feedback.sin_port = htons(47003);
    relay_feedback.sin_addr.s_addr = inet_addr("127.0.0.1");

    unsigned char buf[164];
    for (;;) {
        ssize_t n = recvfrom(in_fd, buf, sizeof(buf), 0, NULL, NULL);
        if (n <= 0) continue;

        uint32_t net_seq;
        memcpy(&net_seq, buf, 4);
        uint32_t host_seq = ntohl(net_seq);
        
        if (host_seq & 0x80000000) {
            // -- FEC Packet Arrived --
            uint32_t odd_seq = host_seq & 0x7FFFFFFF;
            fec_map[odd_seq] = std::vector<uint8_t>(buf, buf + n);
            recover_if_possible(odd_seq);
        } else {
            // -- Normal Packet Arrived --
            jb[host_seq] = std::vector<uint8_t>(buf, buf + n);
            forward_packet(host_seq, jb[host_seq]);
            
            // NACK GAP DETECTION: Request anything we skipped
            if (host_seq > max_seen) {
                // Prevent massive loops if a massive delay spike occurs
                uint32_t start = (host_seq > max_seen + 50) ? host_seq - 50 : max_seen + 1;
                for (uint32_t m = start; m < host_seq; ++m) {
                    if (!forwarded[m]) {
                        uint32_t nack = htonl(m);
                        sendto(feedback_fd, &nack, 4, 0, (struct sockaddr *)&relay_feedback, sizeof(relay_feedback));
                    }
                }
                max_seen = host_seq;
            }

            uint32_t odd_seq = (host_seq % 2 == 0) ? host_seq + 1 : host_seq;
            recover_if_possible(odd_seq);
        }
    }
    return 0;
}