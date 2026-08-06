#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "backend.h"
#include "target.h"
#include "rtl.h"
#include "insn-config.h"
#include "recog.h"
#include "function.h"
#include "memmodel.h"
#include "emit-rtl.h"
#include "tm_p.h"
#include "tree-pass.h"
#include "df.h"
#include "insn-addr.h"

/* Creating doloop_begin patterns fully formed with a named pattern
   reusults in the labels they use to refer to the loop start being
   removed from the insn list during loop_done pass, so instead we
   put the doloop_end insn in the place of the label, and patch this
   up after the loop_done pass.
   Also, while being at that, replace the pseudo reg used for the
   counter in doloop_begin / doloop_end by the appropriate hard register,
   since lra doesn't find the right solution.  */

namespace {

const pass_data pass_data_riscv_doloop_begin =
{
  RTL_PASS, /* type */
  "riscv_doloop_begin", /* name */
  OPTGROUP_LOOP, /* optinfo_flags */
  TV_LOOP_DOLOOP, /* tv_id */
  0, /* properties_required */
  0, /* properties_provided */
  0, /* properties_destroyed */
  0, /* todo_flags_start */
  0, /* todo_flags_finish */
};

class pass_riscv_doloop_begin : public rtl_opt_pass
{
public:
  pass_riscv_doloop_begin (gcc::context *ctxt)
    : rtl_opt_pass (pass_data_riscv_doloop_begin, ctxt)
  {}

  /* opt_pass methods: */
  virtual bool gate (function *)
    {
      return TARGET_XCVHWLP
	&& flag_branch_on_count_reg && optimize > 0;
    }
  virtual unsigned int execute (function *);
}; // class pass_riscv_doloop_begin

unsigned int
pass_riscv_doloop_begin::execute (function *)
{
  for (rtx_insn *insn = get_insns (); insn; insn = NEXT_INSN (insn))
    {
      if (!NONJUMP_INSN_P (insn)
	  || recog_memoized (insn) != CODE_FOR_doloop_begin_i)
	continue;
      rtx *lref_loc = &SET_SRC (XVECEXP (PATTERN (insn), 0, 0));
      rtx_insn *end_insn
	= as_a <rtx_insn *> (XEXP (XVECEXP (*lref_loc, 0, 0), 0));
      rtx pat = PATTERN (end_insn);
      rtx start_label_ref = XEXP (SET_SRC (XVECEXP (pat, 0, 0)), 1);
      start_label_ref
	= gen_rtx_LABEL_REF (SImode, label_ref_label (start_label_ref));
      *lref_loc = start_label_ref;
      add_label_op_ref (insn, start_label_ref);
      rtx *reg_loc0 = &SET_DEST (XVECEXP (PATTERN (insn), 0, 2));
      rtx *reg_loc1 = &XEXP (XEXP (SET_SRC (XVECEXP (pat, 0, 0)), 0), 0);
      rtx *reg_loc2 = &XEXP (SET_SRC (XVECEXP (pat, 0, 1)), 0);
      rtx *reg_loc3 = &SET_DEST (XVECEXP (pat, 0, 1));
      gcc_assert (rtx_equal_p (*reg_loc0, *reg_loc1));
      gcc_assert (rtx_equal_p (*reg_loc0, *reg_loc2));
      gcc_assert (rtx_equal_p (*reg_loc0, *reg_loc3));
      rtx start_reg = SET_DEST (XVECEXP (PATTERN (insn), 0, 0));
      rtx hreg = gen_rtx_REG (SImode,
			      LPCOUNT0_REGNUM
			      + REGNO (start_reg) - LPSTART0_REGNUM);
      *reg_loc0 = hreg;
      *reg_loc1 = hreg;
      *reg_loc2 = hreg;
      *reg_loc3 = hreg;
      df_insn_rescan (insn);
      df_insn_rescan (end_insn);
    }

  return 0;
}

} // anon namespace

rtl_opt_pass *
make_pass_riscv_doloop_begin (gcc::context *ctxt)
{
  return new pass_riscv_doloop_begin (ctxt);
}

/* We'd like to check that there's no flow control inside the loop
   except for nested HW loops and the final branch back to the loop latch.
   However, we can't do that becaue we are not being passed the loop
   structure.
   Likewise, if there is a large loop that has hardly any iterations,
   the loop setup can't be amortized, but we can't test here if the
   loop is large.  */
bool
riscv_can_use_doloop_p (const widest_int &, const widest_int &,
			unsigned int loop_depth, bool entered_at_top)
{
  if (!TARGET_XCVHWLP)
    return false;
  if (loop_depth > 2)
    return false;
  if (!entered_at_top)
    return false;
  return true;
}

/* The only control flow allowed inside a HW loop is another HW loop,
   ebreak, and ecall.  */
const char *
riscv_invalid_within_doloop (const rtx_insn *insn)
{
  if (CALL_P (insn))
    return "Function call in the loop.";
  /* Alas, the jump at the end of the loop is considered part of the loop,
     and there's no good way here to distinguish it from interspersed control
     flow.  We have to leave it to the doloop_end expander to analyze the loop
     again.  */
#if 0
  if (JUMP_P (insn) && recog_memoized (const_cast <rtx_insn *> (insn)) != CODE_FOR_doloop_end_i)
    return "Jump in loop.";
#endif

  return NULL;
}

/* Return the number of 4-byte units we must conservatively assume
   INSN occupies when estimating whether a PC-relative hardware loop
   offset (uimm12 << 2, i.e. at most 4095 words forward) fits.
   Ordinary insns are at most 4 bytes on RV32; inline asm statements
   can expand to arbitrarily many instructions, so account for them
   using their estimated instruction count.  */
static unsigned
hwloop_insn_units (rtx_insn *insn)
{
  rtx pat = PATTERN (insn);
  const char *templ;

  if (GET_CODE (pat) == ASM_INPUT)
    templ = XSTR (pat, 0);
  else if (asm_noperands (pat) >= 0)
    templ = decode_asm_operands (pat, NULL, NULL, NULL, NULL, NULL);
  else
    return 1;

  int n = templ ? asm_str_count (templ) : 1;
  return n > 0 ? (unsigned) n : 1;
}

/* Starting at INSN, walk forward up to COUNT insn-units looking for the
   doloop_end_i that closes the loop whose start label is START_LAB
   (the branch-back target, a real shared label).  Return the remaining
   count if found, else 0.  We match on the start label rather than the
   loop-end operand, because the end operand is an unplaced placeholder
   (null / uid-0) and cannot be compared by identity.  */
static unsigned
doloop_end_range_check (rtx_insn *insn, rtx_insn *start_lab, unsigned count)
{
  for (; count > 0; insn = NEXT_INSN (insn))
    {
      if (insn == NULL_RTX)
	return 0;
      if (!active_insn_p (insn))
	continue;
      if (recog_memoized (insn) == CODE_FOR_doloop_end_i)
	{
	  rtx ite = SET_SRC (XVECEXP (PATTERN (insn), 0, 0));
	  rtx tgt = XEXP (ite, 1);
	  if (GET_CODE (tgt) == LABEL_REF
	    && label_ref_label (tgt) == start_lab)
	    break;
        }
      unsigned units = hwloop_insn_units (insn);
      if (units >= count)
	return 0;
      count -= units;
    }
  return count;
}

/* Return true if LABEL lies *ahead* of INSN and close enough that the
   12-bit unsigned word offset of cv.starti / cv.endi
   (lpstart/lpend = PC + (uimm12 << 2), i.e. at most 4095 words
   forward) is guaranteed to be able to address it.  MAX_UNITS is the
   conservative insn budget to assume (4095 after reload, fewer before
   reload to leave headroom for spill code).
   Walking forward also establishes the direction: the offset field is
   unsigned, so a label behind INSN can never be addressed by the
   immediate forms, no matter how close it is.  */
bool
hwloop_label_offset_in_range_p (rtx uncast_insn, rtx label_ref,
				unsigned max_units)
{
  rtx_insn *insn = as_a <rtx_insn *> (uncast_insn);
  if (GET_CODE (label_ref) == UNSPEC)
    label_ref = XVECEXP (label_ref, 0, 0);
  rtx_insn *label = label_ref_label (label_ref);

  for (rtx_insn *scan = insn; ; scan = NEXT_INSN (scan))
    {
      if (scan == NULL)
	/* Fell off the insn chain: LABEL is behind INSN.  */
	return false;
      if (scan == label)
	return true;
      if (scan == insn || !active_insn_p (scan))
	continue;
      unsigned units = hwloop_insn_units (scan);
      if (units >= max_units)
	return false;
      max_units -= units;
    }
}

/* As hwloop_label_offset_in_range_p, but for the loop *end*.  The end
   label is an unplaced placeholder: it never appears in the insn chain
   (doloop_end_i emits it itself via its "%4:" template), so scanning for
   it always falls off the chain and reports "out of range", forcing the
   address into a register.  Measure instead to the doloop_end_i that
   closes this loop, identified by its branch-target (loop start) label
   START_LAB -- that insn is where the end label will be emitted.  */
bool
hwloop_end_offset_in_range_p (rtx uncast_insn, rtx_insn *start_lab,
			      unsigned max_units)
{
  rtx_insn *insn = as_a <rtx_insn *> (uncast_insn);
  for (rtx_insn *scan = insn; ; scan = NEXT_INSN (scan))
    {
      if (scan == NULL)
	return false;
      if (scan != insn && active_insn_p (scan)
	  && recog_memoized (scan) == CODE_FOR_doloop_end_i)
	{
	  rtx ite = SET_SRC (XVECEXP (PATTERN (scan), 0, 0));
	  rtx tgt = XEXP (ite, 1);
	  if (GET_CODE (tgt) == LABEL_REF
	      && label_ref_label (tgt) == start_lab)
	    return true;
	}
      if (scan == insn || !active_insn_p (scan))
	continue;
      unsigned units = hwloop_insn_units (scan);
      if (units >= max_units)
	return false;
      max_units -= units;
    }
}

static bool
hwloop_valid_label_p (rtx ref)
{
  if (GET_CODE (ref) == UNSPEC)
    ref = XVECEXP (ref, 0, 0);
  if (GET_CODE (ref) != LABEL_REF)
    return false;
  rtx lab = XEXP (ref, 0);
  return lab && LABEL_P (lab) && INSN_UID (lab) != 0;   /* uid 0 == unplaced placeholder */
}

/* Determine if we can implement the loop setup MD_INSN with cv.setupi,
   considering the hardware loop starts at the labels in the LABEL_REFs
   START_REF and END_REF.  */

bool
hwloop_setupi_p (rtx md_insn, rtx start_ref, rtx end_ref)
{
  if (!hwloop_valid_label_p (start_ref))
    return false;
  rtx_insn *insn = as_a <rtx_insn *> (md_insn);
  if (GET_CODE (start_ref) == UNSPEC)
    start_ref = XVECEXP (start_ref, 0, 0);
  rtx_insn *start = label_ref_label (start_ref);

  if (GET_CODE (end_ref) == UNSPEC
      && XINT (end_ref, 1) == UNSPEC_CV_LP_END_12
      && !REG_P (SET_SRC (XVECEXP (PATTERN (insn), 0, 2))))
    return false;

  /* The the loop must directly follow the cv.setupi instruction.  */
  if (next_active_insn (insn) != next_active_insn (start))
    return false;

  /* Loops with >= 4K instructions can't be setup with cv.setupi .  */
  if (doloop_end_range_check (insn, start, 4095) == 0)
    return false;

  return true;
}

/* Called from the output templates of the *cv_start / *cv_end
   patterns when they are about to emit cv.starti / cv.endi.  Once
   shorten_branches has computed instruction addresses, verify that
   the PC-relative target OP (a LABEL_REF) lies ahead of INSN and
   within the 12-bit unsigned word offset (uimm12 << 2).  Basic block
   reordering runs after the doloop splits, so this is the last line
   of defense: an unencodable offset here is a compiler bug, and a
   clean ICE beats emitting an instruction that the assembler rejects
   or silently truncates.  Note that the addresses computed by
   shorten_branches over-estimate insn sizes when RVC is in use, so a
   distance that passes here can only shrink in the final binary.  */
void
corev_check_hwloop_offset (rtx_insn *insn, rtx op)
{
  if (GET_CODE (op) != LABEL_REF || !INSN_ADDRESSES_SET_P ())
    return;
  rtx_insn *lab = label_ref_label (op);
  if (!lab || INSN_UID (lab) == 0)
    return;
  HOST_WIDE_INT src = INSN_ADDRESSES (INSN_UID (insn));
  HOST_WIDE_INT dst = INSN_ADDRESSES (INSN_UID (lab));
  HOST_WIDE_INT offset = dst - src;
  if (offset < 0 || offset > 4095 * 4)
    fatal_insn ("hardware loop label out of range for PC-relative "
		"hardware loop instruction", insn);
}

void
add_label_op_ref (rtx_insn *insn, rtx label)
{
  if (GET_CODE (label) == LABEL_REF)
    label = label_ref_label (label);
  add_reg_note (insn, REG_LABEL_OPERAND, label);
  ++LABEL_NUSES (label);
}

/* Splitting doloop_begin_i into cv.starti / cv.endi / cv.counti (or
   loading the operands into registers for cv.start / cv.end /
   cv.count) gives up the single-insn cv.setupi / cv.setup forms and
   creates PC-relative references that later code motion (bbro,
   sched2) can invalidate.  We therefore keep doloop_begin_i intact
   until the post-sched2 run of the riscv_doloop_ranges pass has had a
   chance to glue it back to the loop start; only then is splitting
   enabled, and the pass performs it itself via split_all_insns.  */
bool riscv_hwloop_splitting_p;

/* Return true if INSN (a doloop_begin_i whose loop count operand is
   COUNT) can be moved forward so it immediately precedes LABEL, the
   loop start.  This requires a straight-line NEXT_INSN chain from
   INSN to LABEL: crossing a label would make the setup execute on
   join paths that did not execute it before, and crossing a jump or
   call would move it across control flow.  The crossed insns must
   neither modify the register the loop count is read from (the setup
   would then read a clobbered value) nor touch any of the hardware
   loop registers (e.g. a nested loop's own setup).  */
static bool
hwloop_safe_to_move_p (rtx_insn *insn, rtx_insn *label, rtx count)
{
  for (rtx_insn *scan = NEXT_INSN (insn); scan != label;
       scan = NEXT_INSN (scan))
    {
      if (scan == NULL)
	/* Fell off the insn chain: LABEL is behind INSN.  */
	return false;
      if (LABEL_P (scan) || BARRIER_P (scan))
	return false;
      if (!INSN_P (scan))
	continue;
      if (JUMP_P (scan) || CALL_P (scan))
	return false;
      if (REG_P (count) && reg_set_p (count, scan))
	return false;
      if (refers_to_regno_p (LPSTART0_REGNUM, LPCOUNT1_REGNUM + 1,
			     PATTERN (scan), NULL))
	return false;
    }
  return true;
}


/* Before register allocation, we need to know if a cv.setupi instruction
   might need to replaced with instructions that use an extra scratch
   register becasue the labels are out of range.  If we split into
   cv.starti / cv.endi / cv.counti, all three parameters can use a
   12 bit immediate.  Considering the instructions inside the loop,
   we got a three-address machine, so a typical instruction has three
   operands, each of which might need reloading.  To load or store a
   register from a stack slot on a 32 bit RISC-V, worst case we might
   need a LUI and a load or store instruction.  Thus seven instruction
   after reload for one instruction before reload.  The 12 bit unsigned
   offset allows 4095 instructions, so for a safe number before reload,
   we divide by seven to arrive at 585.  That seems a comfortable number
   that we don't have to worry too much about pessimizing the code when
   reserve a scratch register when the loop gets that big.

   For performance, we like to use sv.setupi or at least cv.setup where
   possible, as it is only a single instruction; we assume that usually,
   there will be no reloads for a HW loop if currently fit into the 5 bit
   immeidate range, as that makes them a small inner loop.

   loop start not immediatly following -> need to split
   otherwise, if loop end won't fit in u5, probably in u12 -> aim for cv.setup
   otherwise, if loop count won't fit in u12 -> aim for cv.setup
   loop end might not fit into u12 after reload -> need scratch register
    in case end needs to be loaded with cv.end .  */

namespace {

const pass_data pass_data_riscv_doloop_ranges =
{
  RTL_PASS, /* type */
  "riscv_doloop_ranges", /* name */
  OPTGROUP_LOOP, /* optinfo_flags */
  TV_LOOP_DOLOOP, /* tv_id */
  0, /* properties_required */
  0, /* properties_provided */
  0, /* properties_destroyed */
  0, /* todo_flags_start */
  0, /* todo_flags_finish */
};

class pass_riscv_doloop_ranges : public rtl_opt_pass
{
public:
  pass_riscv_doloop_ranges (gcc::context *ctxt)
    : rtl_opt_pass (pass_data_riscv_doloop_ranges, ctxt)
  {}

  /* opt_pass methods: */
  virtual bool gate (function *)
    {
      return TARGET_XCVHWLP
	&& flag_branch_on_count_reg && optimize > 0;
    }
  virtual unsigned int execute (function *);

  opt_pass *clone ()
  {
    return new pass_riscv_doloop_ranges (m_ctxt);
  }
}; // class pass_riscv_doloop_ranges

/* Look for doloop_begin_i patterns and make sure start labels are
   appropriatly encapsulated or non-encapsulated in UNSPECs to show
   if they satisfy offset range requirements.
   We run this once just before register allocation and once afterwards,
   so we can't just formulate this as a branch shortening problem.
   In the post-reload pass, also add doloop_align if necessary.  */
unsigned int
pass_riscv_doloop_ranges::execute (function *)
{
  bool saw_doloop_p = false;

  /* Before register allocation, splitting must stay disabled; the
     post-sched2 run below re-enables it once insn placement is
     final.  */
  if (!reload_completed)
    riscv_hwloop_splitting_p = false;

  for (rtx_insn *insn = get_insns (); insn; insn = NEXT_INSN (insn))
    {
      if (!NONJUMP_INSN_P (insn)
	  || recog_memoized (insn) != CODE_FOR_doloop_begin_i)
	continue;
      saw_doloop_p = true;
      rtx *lref_s_loc = &SET_SRC (XVECEXP (PATTERN (insn), 0, 0));
      rtx *lref_e_loc = &SET_SRC (XVECEXP (PATTERN (insn), 0, 1));
      rtx lp_count =        SET_SRC (XVECEXP (PATTERN (insn), 0, 2));
      rtx scratch =     SET_DEST (XVECEXP (PATTERN (insn), 0, 3));
      rtx start_label_ref = *lref_s_loc;
      rtx end_label_ref = *lref_e_loc;
      if (GET_CODE (start_label_ref) == UNSPEC)
	start_label_ref = XVECEXP (start_label_ref, 0, 0);
      if (GET_CODE (end_label_ref) == UNSPEC)
	end_label_ref = XVECEXP (end_label_ref, 0, 0);

      if (reload_completed
	  && (next_active_insn (label_ref_label (start_label_ref))
	      != next_active_insn (insn))
	  && hwloop_safe_to_move_p (insn,
				    label_ref_label (start_label_ref),
				    lp_count))
	{
	  /* Register allocation spill code, sched2 or block reordering
	     put insns in between the doloop_begin_i and the loop start.
	     Move the doloop_begin_i back to the start of the loop so
	     that the single-insn cv.setup / cv.setupi forms remain
	     usable; hwloop_safe_to_move_p has verified that the crossed
	     insns neither clobber the count register nor touch the
	     hardware loop registers, and that no control flow is
	     crossed.
	     ??? We could allow the doloop_begin_i pattern to read the count
	     from memory (using clobber and splitter to fix that up) to have
	     a better chance to get code that allows the doloop_begin_i to
	     be moved back to the start of the loop.
	     ??? Much better would be to have a hook or other mechanism to
	     prevent reload / lra from inserting spill code between
	     doloop_begin_i and the loop start.  */
	  rtx_insn *next = NEXT_INSN (insn);
	  rtx_insn *after = PREV_INSN (label_ref_label (start_label_ref));

	  remove_insn (insn);
	  SET_PREV_INSN (insn) = NULL_RTX;
	  SET_NEXT_INSN (insn) = NULL_RTX;
	  emit_insn_after (insn, after);
	  df_insn_rescan (insn);

	  insn = next;
	  continue;
	}

      if (next_active_insn (label_ref_label (start_label_ref))
	  != next_active_insn (insn))
	{
	  /* Only promise a 12-bit offset if the start label really is
	     ahead of INSN and within reach of the unsigned PC-relative
	     offset field of cv.starti; otherwise fall back to a bare
	     LABEL_REF so that the doloop_begin_i split loads the label
	     address into the scratch register and uses cv.start.  */
	  if (GET_CODE (*lref_s_loc) == UNSPEC
	      && hwloop_label_offset_in_range_p (insn, start_label_ref,
						 reload_completed
						 ? 4095 : 585))
	    *lref_s_loc = gen_rtx_UNSPEC (SImode,
					  gen_rtvec (1, start_label_ref),
					  UNSPEC_CV_LP_START_12);
	  else
	    *lref_s_loc = start_label_ref;
	  /* We must not emit an insn outside of basic blocks, so
	     emit the align after the  NOTE_INSN_BASIC_BLOCK note.  */
	  rtx_insn *after = NEXT_INSN (label_ref_label (start_label_ref));
	  if (reload_completed && TARGET_RVC)
	    emit_insn_after (gen_doloop_align (), after);
	}
      else
	{
	  if (GET_CODE (*lref_s_loc) != UNSPEC)
	    *lref_s_loc = gen_rtx_UNSPEC (SImode,
					  gen_rtvec (1, start_label_ref),
					  UNSPEC_CV_FOLLOWS);
	  if (reload_completed && TARGET_RVC)
	    emit_insn_before (gen_doloop_align (), insn);
	}

      unsigned count = (reload_completed ? 4095 : 585);
      unsigned rest = doloop_end_range_check (insn, 
		                  label_ref_label (start_label_ref),
				  count);

      if (rest)
	{
	  /* Check if an unsigned 5 bit offset is enough.  */
	  bool short_p = count - rest <= 31;
	  HOST_WIDE_INT val
	    = short_p ? UNSPEC_CV_LP_END_5 : UNSPEC_CV_LP_END_12;
	  if (GET_CODE (*lref_e_loc) != UNSPEC
	      || XINT (*lref_e_loc, 1) != val)
	    *lref_e_loc
	      = gen_rtx_UNSPEC (SImode, gen_rtvec (1, end_label_ref), val);
	}
      else
	*lref_e_loc = end_label_ref;
    }

  /* This run happens after sched2; no pass that moves insns runs
     later, so the setup insns that we could glue back to their loop
     will now be emitted as cv.setup / cv.setupi.  Enable splitting
     and split whatever could not be glued (out-of-range labels,
     spaghetti layout) into the cv.start(i) / cv.end(i) / cv.count(i)
     sequences.  */
  if (reload_completed)
    {
      riscv_hwloop_splitting_p = true;
      if (saw_doloop_p)
	split_all_insns ();
    }

  return 0;
}

} // anon namespace

rtl_opt_pass *
make_pass_riscv_doloop_ranges (gcc::context *ctxt)
{
  return new pass_riscv_doloop_ranges (ctxt);
}

/* Return alignment requested for a label as a power of two.
   We can't put doloop_align instructions before doloop start labels lest
   they end up outside of basic blocks in case there's a preceding BARRIER,
   so we put them after the label.  However, the label must be aligned.  */
int
corev_label_align (rtx_insn *label)
{
  rtx_insn *next = label;
  do
    next = NEXT_INSN (next);
  while (next && NOTE_P (next));
  if (next && NONJUMP_INSN_P (next)
      && recog_memoized (next) == CODE_FOR_doloop_align)
    return 2;
  return 0;
}
