"""Generate a directly threaded native clipping function from local VU1 code.

The decoded operations retain the exact arithmetic/flag pipeline helpers.
Generated instruction words stay under build/, never in tracked source.
The native kernels run only where micro memory holds the words they were written from
(native_code_compare, port/runtime/vu0_interp.c); like the AOT blocks' (port/tools/vu1_aot.py),
those words are copied from the player's disc at start, not kept in dhd.exe.
"""
import os
from pathlib import Path
import struct
from vudis import walk
from vu1_aot import WORDS_MACRO, FiniteOperands, ReadyOperands, blocks, fill_function, fills, lazy_flags, program
ROOT = Path(__file__).resolve().parents[2]

def generate(destination, early_flags=True):
    destination=Path(destination).resolve()
    if (ROOT/'build').resolve() not in destination.parents:  # build/ may be a junction off C:
        raise ValueError('generated clipping code must stay under build/')
    data=Path(os.environ.get('DHD_VUTEXT',ROOT/'assets/vutext.bin')).read_bytes()
    pairs,where=program(data,0x269c50)
    selected=[p for p in sorted(pairs) if 0x1948<=p*8<=0x1ad8 or 0x1cc0<=p*8<=0x2730]
    lines=['/* Generated locally; do not commit. */']+WORDS_MACRO
    entries=[]
    for name,start,end in [
        ('loop',0x170,0x260),
        ('zero',0x270,0x280),
        ('zero1638',0x17c0,0x17e0),
        ('zero17e0',0x1938,0x1948),
        ('loop20c',0x1110,0x11d8),
        ('loop260',0x13a0,0x1420),
        ('loop1638',0x1708,0x17b0),
        ('loop17e0',0x18b0,0x1928),
        ('loop800',0x8b0,0x938),
        ('loop0798',0x798,0x7e0),
        ('loop3140',0x3140,0x3180),
        ('loop468',0x468,0x578),
        ('loop1430',0x1510,0x1638),
        ('loop2dc8',0x2f00,0x2fc8),
        ('tail2dc8',0x2fc8,0x2fd8),
        ('giftail',0xff8,0x1040),
        ('clip1',0x1948,0x1ae0),
        ('clip2',0x1cc0,0x2738),
    ]:
        words=[w for p in range(start//8,end//8) for w in pairs[p]]
        lines+=[f'AOT_WORDS(native_{name}_words, {len(words)}, '+','.join(f'0x{w:08x}u' for w in words)+')']
        entries+=fills(f'native_{name}_words',data,[where[p] for p in range(start//8,end//8)],words)
    # Particle overlay A uses its own word provenance; keep overlay B's pairs
    # below for the directly threaded clipping emitter.
    particle_pairs,particle_where=program(data,0x265c00)
    for name,start,end in [('loop0e10',0xee0,0xfd0),('tail0e10',0xfd0,0x1010),
                           ('loop2e48',0x2f90,0x30c0),('tail2e48',0x30c0,0x30f8)]:
        words=[w for p in range(start//8,end//8) for w in particle_pairs[p]]
        lines+=[f'AOT_WORDS(native_{name}_words, {len(words)}, '+','.join(f'0x{w:08x}u' for w in words)+')']
        entries+=fills(f'native_{name}_words',data,[particle_where[p] for p in range(start//8,end//8)],words)
    lines+=fill_function('native_words',entries)
    # This closed clip program only reads MAC at 1970 (SUBA at 1950, four
    # issue slots earlier), and CLIP at 1A58/68/78/88/98 and 2600. The newest
    # CLIP before 1A58 is at 1A38, four slots earlier. 2600 is reached through
    # 25F8 after the caller's branch/delay slot and the 25F8 pair; every caller
    # issued its CLIP at least four slots earlier. No status flag reads occur.
    reads={pc*8 for pc in selected if pairs[pc][1]>>31==0 and pairs[pc][0]>>25 in range(0x10,0x1d)}
    assert reads == {0x1970,0x1a58,0x1a68,0x1a78,0x1a88,0x1a98,0x2600}, reads
    # The private frame below preserves pending P unchanged and caches physical
    # data memory, like its native callers. Reject a routine that reads/writes P.
    special = {(lo >> 6 & 31) * 4 + (lo & 3)
               for pc in selected for lo, hi in [pairs[pc]]
               if not hi >> 31 and lo >> 25 == 0x40 and lo & 0x3f >= 0x3c}
    assert not special & {0x64, 0x70, 0x71, 0x72, 0x73, 0x76, 0x78, 0x79, 0x7a, 0x7b}, special
    # FMACs whose MAC update the read at 1970 cannot see commit lazily (vu1_aot.py lazy_flags,
    # finding 090 VU1R), analysed on this closed routine alone: every way out of it (the return
    # to the kernel, any pair outside it) counts as reaching a read. drain_flags at the return
    # folds them in before the kernel reads the status again.
    lazy=lazy_flags({pc:pairs[pc] for pc in selected}) if early_flags else set()
    # Use the same local proofs as AOT, restarting at every possible branch
    # target/return and after each branch delay slot. No caller-state assumption.
    starts = blocks({pc:pairs[pc] for pc in selected})
    proofs, stalls, finite, ready = {}, {}, FiniteOperands(), ReadyOperands()
    for pc in selected:
        if pc in starts: finite, ready = FiniteOperands(), ReadyOperands()
        proofs[pc] = finite.step(*pairs[pc])
        stalls[pc] = ready.step(*pairs[pc])
    lines+=['static inline __attribute__((always_inline)) void native_clip_commit(Vu *u, Upper *up) {',
            'if(up->flags==1){unsigned m=up->mac; unsigned st=(m&15?1:0)|(m&240?2:0)|(m&3840?4:0)|(m&61440?8:0);',
            'u->mac=m;u->status=(u->status&~15u)|st|(st<<6);u->lazy_valid=0;}',
            'else if(up->flags==2){u->clip=u->clip_next=((u->clip<<6)|up->clip_bits)&0xffffff;}',
            'up->flags=0;commit_upper(u,up,0);}',
            'static inline __attribute__((always_inline)) unsigned native_clip_impl(Vu *restrict u, unsigned resume, int sticky_complete, ps2_q128 *data) {','unsigned count=0; int target=-1,bad=0; Upper up;','goto L1948;']
    def emit(pc):
        lo,hi=pairs[pc]
        out=[f'VU1(u); {stalls[pc]} apply_flags(u); update_q(u);',
             f'if(upper_proven(u,0x{hi:08x}u,&up,{proofs.get(pc,0)})<0) return 0;']
        if hi>>31:
            out+=[f'{{uint32_t word=0x{lo:08x}u;memcpy(&u->c->i,&word,4);}}','target=-1;']
        else:
            out+=[f'target=lower_cached(u,0x{lo:08x}u,{pc},&bad,data); if(bad)return 0;']
        commit='commit_upper_lazy_proven(u,&up,0,sticky_complete);' if pc in lazy else 'native_clip_commit(u,&up);' if early_flags else f'commit_upper(u,&up,{lo>>16&31});'
        out += [commit + 'u->cycle++;count++;']
        return out
    branches=(0x20,0x21,0x24,0x25,0x28,0x29,0x2c,0x2d,0x2e,0x2f)
    for pc in selected:
        lo,hi=pairs[pc]
        lines+=[f'L{pc*8:04X}:', 'if(count>=MAX_STEPS)return 0;']+emit(pc)
        if not hi>>31 and lo>>25 in branches:
            lines+=['{ int saved_target=target;']+emit(pc+1)
            if lo>>25 in (0x24,0x25):
                lines+=['target=saved_target;goto indirect; }']
            else:
                imm=lo&0x7ff
                if imm&0x400:imm-=0x800
                lines += [f'if(saved_target>=0)goto L{(pc+1+imm)*8:04X}; goto L{(pc+2)*8:04X}; }}']
        else:
            # Delay slots emitted above can also be jumped to independently.
            lines += [f'goto L{(pc+1)*8:04X};'] if pc+1 in selected else ['return 0;']
    lines+=['indirect:','if(target==(int)resume){drain_flags(u);update_q(u);return count;}','switch(target){']
    # Only computed returns used by this closed subprogram; default rejects corruption.
    for pc in selected:
        lo,hi=pairs[pc]
        if not hi>>31 and lo>>25 in (0x21,0x25):
            lines+=[f'case {(pc+2)}:goto L{(pc+2)*8:04X};']
    lines+=['default:return 0;','}','}']
    lines+=['static unsigned native_clip_run(Vu *u, unsigned resume) {',
            '/* A private pipeline cannot alias the CPU or its data memory. This',
            '   lets the compiler retain ready times and lazy flags across stores.',
            '   Every caller NATIVE_SAVE advances 64 cycles and empties flags/Q:',
            '   input register ready times and the integer bypass have expired.',
            '   Return drains flags/Q; the caller again advances 64 cycles before',
            '   interpreted/AOT code can observe the frame. Keep expired times in',
            '   the caller instead of copying the ready arrays and empty queue.',
            '   The code-word guard and the assertions above bound this proof.',
            '   The frame keeps its ready times from call to call (clearing their',
            '   512 bytes was about a fifth of this routine): each is at most',
            '   the cycle it ended at + 4 (an FMAC result), so while the cycle of',
            '   the caller has reached that bound every one has expired, as zeroed',
            '   ones had. That cycle only grows within a run, and a run starts',
            '   64 past the last (run_state), so the clear is left to the rare call',
            '   that restarts the cycle at 0 or meets a frame that gave up early.',
            '   restrict restates that the frame is private, as a local was. */',
            'static _Thread_local Vu frame; static _Thread_local uint32_t frame_expired;',
            'if (u->cycle < frame_expired) memset(frame.component_ready, 0, sizeof frame.component_ready);',
            'Vu *restrict const local_ = &frame;',
            '#define local (*local_)',
            'local.c = u->c; local.cycle = u->cycle; local.unit = 1;',
            'local.code_mask = u->code_mask; local.data_mask = u->data_mask;',
            'local.top = u->top; local.itop = u->itop; local.kick = u->kick; local.opaque = u->opaque;',
            'local.mac = u->mac; local.status = u->status; local.clip = u->clip; local.clip_next = u->clip_next;',
            'local.n_flags = local.flag_head = local.q_ready = 0; local.flag_cycle = local.cycle;',
            'local.q_next = u->q_next; local.q_flags = u->q_flags; local.p_ready = u->p_ready; local.p_next = u->p_next;',
            'local.vi_previous_reg = local.vi_previous_value = local.vi_written_at = local.vu1_used = 0;',
            'local.lazy_sign = local.lazy_zero = _mm_setzero_si128();',
            'local.lazy_valid = local.lazy_used = 0;',
            'ps2_q128 *data = u->c->vu0_mem; if (!data) __builtin_unreachable();',
            'unsigned count = (local.status & 0xc0) == 0xc0 ? native_clip_impl(&local, resume, 1, data)',
            '                                               : native_clip_impl(&local, resume, 0, data);',
            'u->cycle = local.cycle; u->flag_cycle = local.flag_cycle;',
            'u->mac = local.mac; u->status = local.status; u->clip = local.clip; u->clip_next = local.clip_next;',
            'u->q_ready = local.q_ready; u->q_next = local.q_next; u->q_flags = local.q_flags;',
            'u->n_flags = local.n_flags; u->flag_head = local.flag_head;',
            'u->vi_previous_reg = local.vi_previous_reg; u->vi_previous_value = local.vi_previous_value; u->vi_written_at = local.vi_written_at;',
            'u->lazy_sign = local.lazy_sign; u->lazy_zero = local.lazy_zero; u->lazy_valid = local.lazy_valid; u->lazy_used = local.lazy_used;',
            'frame_expired = local.cycle + 4;',
            '#undef local',
            'return count;','}']
    destination.write_text('\n'.join(lines)+'\n')
