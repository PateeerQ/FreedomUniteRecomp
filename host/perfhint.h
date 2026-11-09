/* Android's performance hints (ADPF): tell the system how long each game
 * frame took against its target, so it raises the CPU clocks before the game
 * falls behind instead of after. No-ops elsewhere and before Android 13. */
#ifndef MHFU_PERFHINT_H
#define MHFU_PERFHINT_H

/* Add the calling thread to the hint session (once per thread; cheap after). */
void mhfu_perfhint_thread(void);
/* Once a game frame, on the game thread: fps is the rate it should run at. */
void mhfu_perfhint_frame(int fps);

#endif
