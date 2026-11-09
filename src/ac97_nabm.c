/*
 * ac97_nabm.c -- AC'97 bus-master registers whose reset bit clears itself.
 *
 * DirectSoundCreate resets each audio DMA channel by writing RR ("Reset
 * Registers", bit 1) to the channel's control byte at NABM + 0x0B and waiting
 * for the controller to clear it. MC3's DSOUND (sub_003946AE) does it for the
 * channels at 0xFEC00110 and 0xFEC00170:
 *
 *     mov  byte [0xFEC0011B], 2
 *     mov  cl, [0xFEC0011B]        ; read ONCE: MSVC hoisted the load
 *     and  cl, 2
 *   L: test cl, cl
 *     jne  L                       ; spins forever if that read saw RR
 *
 * On hardware the reset is done before the read. The toolkit's "plain" AC'97
 * mode (POSIX, Switch: no fault handling) clears RR from a background thread
 * after the write, which loses against this single read -- the first watchdog
 * snapshot of MC3 sat exactly here. Windows traps the write instead.
 *
 * DSOUND is lifted with MMIO accessors (MC3_MMIO_SECTIONS), so its register
 * writes reach a device model. This one stores every byte to the same plain
 * memory the runtime already backs the aperture with -- reads and code that is
 * not MMIO-lifted see identical values -- except that RR never lands in a
 * control byte. Same channel sweep as the toolkit's ac97_clear_reset_bits
 * (+0x0B of every 16-byte channel in NABM 0x100..0x17F); RR only, never RPBM.
 *
 * Candidate for the toolkit: it fixes the plain mode for any hoisted poll.
 */
#include <stdint.h>
#include <stdio.h>

#include <xbox/xboxrecomp.h>

#define AC97_NABM_LO 0xFEC00100u
#define AC97_NABM_HI 0xFEC00180u
#define AC97_RR      0x02u

static volatile uint8_t *s_nabm;   /* host address of AC97_NABM_LO */

static uint32_t nabm_read(void *opaque, uint32_t off, unsigned size)
{
    uint32_t v = 0;
    unsigned i;

    (void)opaque;
    for (i = 0; i < size; i++)
        v |= (uint32_t)s_nabm[off + i] << (8 * i);
    return v;
}

static void nabm_write(void *opaque, uint32_t off, uint32_t val, unsigned size)
{
    unsigned i;

    (void)opaque;
    for (i = 0; i < size; i++) {
        uint32_t at = off + i;
        uint8_t b = (uint8_t)(val >> (8 * i));

        if ((at & 0xFu) == 0xBu)    /* a channel's control byte: reset completes now */
            b &= (uint8_t)~AC97_RR;
        s_nabm[at] = b;
    }
}

void mc3_ac97_nabm_install(void)
{
    s_nabm = (volatile uint8_t *)xbox_ptr(AC97_NABM_LO);
    if (xbox_MmioRegister(AC97_NABM_LO, AC97_NABM_HI, nabm_read, nabm_write, NULL) != 0)
        fprintf(stderr, "[BOOT] AC97 bus-master model NOT installed (MMIO table full)\n");
}
