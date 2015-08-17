/*****************************************************************************\
* Qemu Simulation Framework (qsim)                                            *
* Qsim is a modified version of the Qemu emulator (www.qemu.org), couled     *
* a C++ API, for the use of computer architecture researchers.                *
*                                                                             *
* This work is licensed under the terms of the GNU GPL, version 2. See the    *
* COPYING file in the top-level directory.                                    *
\*****************************************************************************/
#include <iostream>
#include <fstream>
#include <iomanip>
#include <string>
#include <thread>

#include <qsim.h>
#include <stdio.h>
#include <capstone.h>
#include <zlib.h>

#include "gzstream.h"
#include "cs_disas.h"
#include "macsim_tracegen.h"

#define DEBUG 0

using Qsim::OSDomain;

using std::ostream;
ogzstream* debug_file;

class InstHandler {
public:
    InstHandler();
    ~InstHandler();
    InstHandler(gzFile& outfile);
    void setOutFile(gzFile outfile);
    void closeOutFile(void);
    bool populateInstInfo(cs_insn *insn, cs_regs regs_read, cs_regs regs_write,
                          uint8_t regs_read_count, uint8_t regs_write_count);
    void populateMemInfo(uint64_t v, uint64_t p, uint8_t s, int w);
    void dumpInstInfo(bool inst_idx);
    void openDebugFile();
    void closeDebugFile();
private:

    trace_info_a64_s inst[2]; 
    bool inst_idx;
    gzFile outfile;
    int m_fp_uop_table[ARM64_INS_ENDING];
    int m_int_uop_table[ARM64_INS_ENDING];

#if DEBUG
#endif
    bool started;
};

InstHandler::InstHandler()
{
    inst_idx = 0;
    started = false;
}

void InstHandler::openDebugFile()
{
#if DEBUG
    debug_file = new ogzstream("debug.log.gz");
#endif
}

void InstHandler::closeDebugFile()
{
#if DEBUG
    if (!debug_file)
      return;
    debug_file->close();
    delete debug_file;
    debug_file = NULL;
#endif
}

InstHandler::~InstHandler()
{
}

void InstHandler::dumpInstInfo(bool inst_idx)
{
}

void InstHandler::setOutFile(gzFile file)
{
    outfile = file;
}

void InstHandler::closeOutFile(void)
{
    gzclose(outfile);
}

void InstHandler::populateMemInfo(uint64_t v, uint64_t p, uint8_t s, int w)
{
    trace_info_a64_s *op = &inst[!inst_idx];
    if (w) {
        if (!op->m_has_st) { /* first write */
          op->m_has_st            = 1;
          op->m_mem_write_size    = s;
          op->m_st_vaddr          = p;
        } else {
          op->m_mem_write_size   += s;
        }
    } else {
        if (!op->m_num_ld) { /* first load */
          op->m_num_ld++;
          op->m_ld_vaddr1         = p;
          op->m_mem_read_size     = s;
        } else if (op->m_ld_vaddr1 + op->m_mem_read_size == p) { /* second load */
          op->m_mem_read_size    += s;
        } else {
          op->m_num_ld++;
          op->m_ld_vaddr2         = p;
        }
    }
#if DEBUG
      if (debug_file) {
          *debug_file << std::endl
                     << (w ? "Write: " : "Read: ")
                     << "v: 0x" << std::hex << v
                     << " p: 0x" << std::hex << p 
                     << " s: " << std::dec << (int)s
                     << " val: " << std::hex << *(uint32_t *)p;
      }
#endif /* DEBUG */

    return;
}

bool InstHandler::populateInstInfo(cs_insn *insn, cs_regs regs_read, cs_regs regs_write, 
                                   uint8_t regs_read_count, uint8_t regs_write_count)
{
    cs_arm64* arm64;
    trace_info_a64_s *op = &inst[inst_idx];
    trace_info_a64_s *prev_op = NULL;

    if (started)
        prev_op = &inst[!inst_idx];
    else
        started = true;

    if (insn->detail == NULL)
        return false;

    arm64 = &(insn->detail->arm64);

    op->m_num_read_regs = regs_read_count;
    op->m_num_dest_regs = regs_write_count;

    for (int i = 0; i < regs_read_count; i++)
        op->m_src[i] = regs_read[i];
    for (int i = 0; i < regs_write_count; i++)
        op->m_dst[i] = regs_write[i];

    op->m_cf_type = 0;
    for (int grp_idx = 0; grp_idx < insn->detail->groups_count; grp_idx++) {
        if (insn->detail->groups[grp_idx] == ARM64_GRP_JUMP) {
            op->m_cf_type = 1;
            break;
        }
    }

    op->m_has_immediate = 0;
    for (int op_idx = 0; op_idx < arm64->op_count; op_idx++) {
        if (arm64->operands[op_idx].type == ARM64_OP_IMM ||
            arm64->operands[op_idx].type == ARM64_OP_CIMM) {
            op->m_has_immediate = 1;
            break;
        }
    }

    op->m_opcode = insn->id;
    op->m_has_st = 0;
    op->m_is_fp = 0;
    for (int op_idx = 0; op_idx < arm64->op_count; op_idx++) {
        if (arm64->operands[op_idx].type == ARM64_OP_FP) {
            op->m_is_fp = 1;
            break;
        }
    }

    op->m_write_flg  = arm64->writeback;

    // TODO: figure out based on opcode
    op->m_num_ld = 0;
    op->m_size = 4;

    // initialize current inst dynamic information
    op->m_ld_vaddr2 = 0;
    op->m_st_vaddr = 0;
    op->m_instruction_addr  = insn->address;
    
    op->m_branch_target = 0;
    int offset = 0;
    if (op->m_cf_type) {
        if (op->m_has_immediate)
            for (int op_idx = 0; op_idx < arm64->op_count; op_idx++) {
                if (arm64->operands[op_idx].type == ARM64_OP_IMM) {
                    offset = (int64_t) arm64->operands[op_idx].imm;
                    op->m_branch_target = op->m_instruction_addr + offset;
                    break;
                }
            }
    }

    op->m_mem_read_size = 0;
    op->m_mem_write_size = 0;
    op->m_rep_dir = 0;
    op->m_actually_taken = 0;

    // auxiliary information for prefetch and barrier instructions
    if (arm64->op_count) {
      if (arm64->operands[0].type == ARM64_OP_PREFETCH)
        op->m_st_vaddr = arm64->operands[0].prefetch;

      if (arm64->operands[0].type == ARM64_OP_BARRIER)
        op->m_st_vaddr = arm64->operands[0].barrier;
    }

    // populate prev inst dynamic information
    if (prev_op) {
      if (op->m_instruction_addr == prev_op->m_branch_target)
        prev_op->m_actually_taken = 1;

        // dump trace for previous op
      gzwrite(outfile, prev_op, sizeof(trace_info_a64_s));
#if DEBUG
      if (debug_file) {
        if (prev_op->m_cf_type)
          *debug_file << " Taken " << prev_op->m_actually_taken << std::endl;
        else
          *debug_file << std::endl;
      }
#endif /* DEBUG */
      memset(prev_op, 0, sizeof(trace_info_a64_s));
    }

#if DEBUG
    if (debug_file) {
      *debug_file << std::endl << std::endl;
      *debug_file << "IsBranch: " << (int)op->m_cf_type
        << " Offset:   " << std::setw(8) << std::hex << offset 
        << " Target:  "  << std::setw(8) << std::hex << op->m_branch_target << " ";
      *debug_file << std::endl;
      *debug_file << a64_opcode_names[insn->id] << 
             ": " << std::hex << insn->address <<
             ": " << insn->mnemonic <<
             ": " << insn->op_str;
      *debug_file << std::endl;
      *debug_file << "Src: ";
      for (int i = 0; i < op->m_num_read_regs; i++)
          *debug_file << std::dec << (int) op->m_src[i] << " ";
      *debug_file << std::endl << "Dst: ";
      for (int i = 0; i < op->m_num_dest_regs; i++)
          *debug_file << std::dec << (int) op->m_dst[i] << " ";
      *debug_file << std::endl;
    } else {
      std::cout << "Writing to a null tracefile" << std::endl;
    }
#endif /* DEBUG */
    inst_idx = !inst_idx;

    return true;
}

class TraceWriter {
public:
  TraceWriter(OSDomain &osd) :
    osd(osd), finished(false), dis(CS_ARCH_ARM64, CS_MODE_ARM)
  { 
    inst_handle = new InstHandler[osd.get_n()];
    osd.set_app_start_cb(this, &TraceWriter::app_start_cb);
    trace_file_count = 0;
    finished = false;
  }

  bool hasFinished() { return finished; }

  int app_start_cb(int c) {
    static bool ran = false;
    int n_cpus = osd.get_n();
    if (!ran) {
      ran = true;
      osd.set_inst_cb(this, &TraceWriter::inst_cb);
      osd.set_mem_cb(this, &TraceWriter::mem_cb);
      osd.set_app_end_cb(this, &TraceWriter::app_end_cb);
    }
    for (int i = 0; i < n_cpus; i++) {
      gzFile tracefile  = gzopen(("trace_" + std::to_string(trace_file_count) + "-" + std::to_string(i) + ".log.gz").c_str(), "w");
      inst_handle[i].setOutFile(tracefile);
    }
    inst_handle[0].openDebugFile();
    trace_file_count++;
    finished = false;

    return 0;
  }

  int app_end_cb(int c)
  {
      std::cout << "App end cb called" << std::endl;
      finished = true;

      for (int i = 0; i < osd.get_n(); i++)
          inst_handle[i].closeOutFile();

      inst_handle[0].closeDebugFile();

      return 0;
  }

  void inst_cb(int c, uint64_t v, uint64_t p, uint8_t l, const uint8_t *b, 
               enum inst_type t)
  {
      cs_insn *insn = NULL;
      uint8_t regs_read_count, regs_write_count;
      cs_regs regs_read, regs_write;

      int count = dis.decode((unsigned char *)b, l, insn);
      insn[0].address = v;
      dis.get_regs_access(insn, regs_read, regs_write, &regs_read_count, &regs_write_count);
      inst_handle[c].populateInstInfo(insn, regs_read, regs_write, regs_read_count, regs_write_count);
#if DEBUG
      *debug_file << " Core: " << std::dec << c;
#endif
      dis.free_insn(insn, count);
      return;
  }

  int mem_cb(int c, uint64_t v, uint64_t p, uint8_t s, int w)
  {
      inst_handle[c].populateMemInfo(v, p, s, w);

      return 0;
  }

private:
  OSDomain &osd;
  bool finished;
  int  trace_file_count;
  InstHandler *inst_handle;
  cs_disas dis;

  static const char * itype_str[];
};

const char *TraceWriter::itype_str[] = {
  "QSIM_INST_NULL",
  "QSIM_INST_INTBASIC",
  "QSIM_INST_INTMUL",
  "QSIM_INST_INTDIV",
  "QSIM_INST_STACK",
  "QSIM_INST_BR",
  "QSIM_INST_CALL",
  "QSIM_INST_RET",
  "QSIM_INST_TRAP",
  "QSIM_INST_FPBASIC",
  "QSIM_INST_FPMUL",
  "QSIM_INST_FPDIV"
};

int main(int argc, char** argv) {
  using std::istringstream;
  using std::ofstream;

  unsigned n_cpus = 1;

  std::string qsim_prefix(getenv("QSIM_PREFIX"));

  // Read number of CPUs as a parameter. 
  if (argc >= 2) {
    istringstream s(argv[1]);
    s >> n_cpus;
  }

  OSDomain *osd_p(NULL);

  if (argc >= 4) {
    // Create new OSDomain from saved state.
    osd_p = new OSDomain(argv[3]);
    n_cpus = osd_p->get_n();
  } else {
    osd_p = new OSDomain(n_cpus, qsim_prefix + "/../arm64_images/vmlinuz");
  }
  OSDomain &osd(*osd_p);

  // Attach a TraceWriter if a trace file is given.
  TraceWriter tw(osd);

  // If this OSDomain was created from a saved state, the app start callback was
  // received prior to the state being saved.
  //if (argc >= 4) tw.app_start_cb(0);

  osd.connect_console(std::cout);

  //tw.app_start_cb(0);
  // The main loop: run until 'finished' is true.
  uint64_t inst_per_iter = 1000000000;
  int inst_run = inst_per_iter;
  while (!(inst_per_iter - inst_run)) {
    inst_run = osd.run(0, inst_per_iter);
    osd.timer_interrupt();
  }

  delete osd_p;

  return 0;
}
