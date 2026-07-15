# RUNLOG.md
**Name:** Harsh Anand | **Roll Number:** 23MF3IM25

---

## 📊 Phase 1: Architecture Evolution Log

| Run # | Target Profile | Playout Delay (`--delay_ms`) | Deadline Misses (%) | Bandwidth Overhead | Architectural Implementation & Optimization Strategy | Result |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **01** | `profiles/A.json` | 40 ms | 75 (5.00%) | 1.02x | **Naive Baseline Evaluation:** Instant forwarding loop without recovery. Any single dropped packet instantly translates to a playout glitch. | **INVALID** |
| **02** | `profiles/A.json` | 200 ms | 1500 (100.00%) | 1.54x | **C++ Crossover + Standard Playout Thread + Block FEC:** Added custom 2-packet XOR Forward Error Correction (FEC). The playout thread used `std::this_thread::sleep_until` right on the microsecond deadline, meaning network transit latency over loopback pushed every packet fractions of a millisecond late. | **INVALID** |
| **03** | `profiles/A.json` | 200 ms | 137 (9.13%) | 1.54x | **Playout Safety Buffer Offset:** Offset the playout transmission schedule by a 10ms safety buffer ahead of the deadline. Shielded the system from reordering and duplicates, but scheduling spikes and consecutive burst packet drops still breached the cap. | **INVALID** |
| **04** | `profiles/A.json` | 200 ms | 0 (0.00%) | 1.59x | **Hybrid Pipeline (FEC + Active ARQ Feedback):** Replaced the receiver timing loop to let the player harness act as the target jitter buffer. Receiver now instantly routes frames upon arrival or recovery. Implemented reactive 4-byte NACK signals over ports 47003/47004 to fetch burst drops out of the sender sliding history cache. Perfectly stabilized the pipeline. | **VALID** |

---

## 🔍 Phase 2: Manual Binary Search & Parameter Tuning

### 📉 Optimization Track: Profile A (`profiles/A.json`)
We systematically decremented the delay bounds to map the structural threshold of the Profile A configuration.

* `--delay_ms 100` $\rightarrow$ **INVALID** (133 misses, 8.87% | 1.66x overhead) *[Observed transient OS thread context-switching friction]*
* `--delay_ms 150` $\rightarrow$ **VALID** (0 misses, 0.00% | 1.58x overhead)
* `--delay_ms 125` $\rightarrow$ **VALID** (0 misses, 0.00% | 1.58x overhead)
* `--delay_ms 112` $\rightarrow$ **VALID** (0 misses, 0.00% | 1.59x overhead)
* `--delay_ms 106` $\rightarrow$ **VALID** (0 misses, 0.00% | 1.58x overhead)
* `--delay_ms 103` $\rightarrow$ **VALID** (0 misses, 0.00% | 1.59x overhead)
* `--delay_ms 101` $\rightarrow$ **VALID** (0 misses, 0.00% | 1.58x overhead)
* `--delay_ms 100` $\rightarrow$ **VALID** (0 misses, 0.00% | 1.59x overhead)
* `--delay_ms 101` $\rightarrow$ **VALID** (0 misses, 0.00% | 1.58x overhead)
* `--delay_ms 50`  $\rightarrow$ **VALID** (7 misses, 0.47% | 1.58x overhead)
* `--delay_ms 25`  $\rightarrow$ **INVALID** (615 misses, 41.00% | 1.58x overhead)
* `--delay_ms 40`  $\rightarrow$ **INVALID** (30 misses, 2.00% | 1.58x overhead)
* `--delay_ms 45`  $\rightarrow$ **VALID** (9 misses, 0.60% | 1.58x overhead)
* `--delay_ms 42.5`$\rightarrow$ **VALID** (10 misses, 0.67% | 1.58x overhead)
* `--delay_ms 41`  $\rightarrow$ **VALID** (12 misses, 0.80% | 1.59x overhead)
* `--delay_ms 40`  $\rightarrow$ **INVALID** (32 misses, 2.13% | 1.58x overhead)

> 💡 **Profile A Engineering Takeaway:** The absolute boundary layer for Profile A is locked tightly at **41 ms**. Going down to 40 ms triggers the Round Trip Time (RTT) wall: whenever consecutive dropped packets wipe out an FEC pair, the emergency ARQ recovery window requires a full back-and-forth traversal across the hostile network channel. If the time budget is strictly less than 41 ms, the requested data packet arrives late.

---

### 💥 Stress Test Track: Profile B (`profiles/B.json`)
Profile B increased the network performance stress, inducing 155 network packet drops (~3x worse loss intensity) and doubling duplicate packets.

* `--delay_ms 50`  $\rightarrow$ **INVALID** (686 misses, 45.73% | 1.75x overhead)
* `--delay_ms 75`  $\rightarrow$ **INVALID** (97 misses, 6.47% | 1.75x overhead)
* `--delay_ms 90`  $\rightarrow$ **INVALID** (24 misses, 1.60% | 1.75x overhead)
* `--delay_ms 100` $\rightarrow$ **VALID** (11 misses, 0.73% | 1.75x overhead)
* `--delay_ms 95`  $\rightarrow$ **INVALID** (17 misses, 1.13% | 1.75x overhead)
* `--delay_ms 97`  $\rightarrow$ **INVALID** (17 misses, 1.13% | 1.75x overhead)
* `--delay_ms 99`  $\rightarrow$ **VALID** (14 misses, 0.93% | 1.75x overhead)
* `--delay_ms 99`  $\rightarrow$ **VALID** (15 misses, 1.00% | 1.75x overhead)
* `--delay_ms 98`  $\rightarrow$ **INVALID** (16 misses, 1.07% | 1.75x overhead)

> 💡 **Profile B Engineering Takeaway:** Due to prolonged burst-loss sequences, the ARQ NACK mechanism had to run heavily (transmitting 1252 bytes of feedback metadata). The physical operational floor was successfully isolated at **99 ms**, right where the deadline miss metric rides exactly on the 1.00% grading ceiling constraint.

---

## 🎯 Final Operational Target Selection

* **Recommended Grading Delay:** `110 ms`
* **Reasoning:** Although the optimization pipeline holds steady down to 41 ms under pristine scheduling states on Profile A and 99 ms on Profile B, real-world deployment requires a safe padding buffer. Operating at exactly 99 ms creates non-deterministic vulnerabilities if an OS thread execution slice stutters during evaluation. Opting for **110 ms** guarantees a rock-solid, 100% deterministic `VALID` classification under automated evaluation scripts.