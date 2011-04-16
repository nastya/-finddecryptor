#include "finder-cycle.h"

using namespace std;

#ifdef FINDER_LOG
	#define LOG (*log)
#else
	#define LOG if (false) cerr
#endif

const uint FinderCycle::maxBackward = 20;
const uint FinderCycle::maxForward = 100;
const uint FinderCycle::maxEmulate = 100;

FinderCycle::FinderCycle(int type) : Finder(type)
{
	regs_known = new bool[RegistersCount];
	regs_target = new bool[RegistersCount];
	Timer::start();
}

FinderCycle::~FinderCycle()
{
	Timer::stop();

	LOG	<< endl << endl
		<< "Time total: " << dec << Timer::secs() << " seconds." << endl
		<< "Time spent on load: " << dec << Timer::secs(TimeLoad) << " seconds." << endl
		<< "Time spent on find: " << dec << Timer::secs(TimeFind) << " seconds." << endl
		<< "Time spent on find_memory_and_jump: " << dec << Timer::secs(TimeFindMemoryAndJump) << " seconds." << endl
		<< "Time spent on launches: " << dec << Timer::secs(TimeLaunches) << " seconds." << endl
		<< "Time spent on backwards traversal: " << dec << Timer::secs(TimeBackwardsTraversal) << " seconds." << endl
		<< "Time spent on emulator launches (total): " << dec << Timer::secs(TimeEmulatorStart) << " seconds." << endl;

	delete[] regs_known;
	delete[] regs_target;
}
void FinderCycle::load(string name, bool guessType) {
	start_positions.clear();
	Finder::load(name, guessType);
}
void FinderCycle::link(const unsigned char *data, uint dataSize, bool guessType) {
	start_positions.clear();
	Finder::link(data, dataSize, guessType);
}
void FinderCycle::launch(int pos)
{
	Timer::start(TimeLaunches);
	LOG << "Launching from position 0x" << hex << pos << endl;
	int a[1000] = {0}, k, num, amount=0;
	uint barrier;
	bool flag = false;
//	Command cycle[256];
	INSTRUCTION inst;
	Timer::start(TimeEmulatorStart);
	emulator->begin(pos);
	Timer::stop(TimeEmulatorStart);
	char buff[10] = {0};
	for (uint strnum = 0; strnum < maxEmulate; strnum++) {
		if (!emulator->get_command(buff)) {
			LOG << " Execution error, stopping instance." << endl;
			Timer::stop(TimeLaunches);
			return;
		}
		num = emulator->get_register(EIP);
		if (!reader->is_valid(num)) {
			LOG << " Reached end of the memory block, stopping instance." << endl;
			Timer::stop(TimeLaunches);
			return;
		}
		get_instruction(&inst, (BYTE *) buff, mode);
		LOG << strnum << "  Command: 0x" << hex << num << ": " << instruction_string(&inst, num) << endl;
		if (!emulator->step()) {
			LOG << " Execution error, stopping instance." << endl;
			Timer::stop(TimeLaunches);
			return;
		}
		get_operands(&inst);
		for (int i = 0; i < RegistersCount; i++) {
			if (regs_target[i]&&regs_known[i]) {
				regs_target[i] = false;
			} else if (regs_target[i]&&!regs_known[i]) {
				for (k=0;k<RegistersCount;k++) {
					if (!regs_target[k]) {
						regs_target[k] = regs_known[k];
					} else {
						regs_known[k] = regs_target[k];
					}
				}
				check(&instructions_after_getpc);
				int em_start = backwards_traversal(pos_getpc);
				if (em_start<0) {
					LOG <<  " Backwards traversal failed (nothing suitable found)." << endl;
					Timer::stop(TimeLaunches);
					return;
				}
				LOG <<  " relaunch (because of " << Registers[i] << "). New position: 0x" << hex << em_start << endl;
				Timer::stop(TimeLaunches);
				return launch(em_start);
			}
		}
		if (strnum >= instructions_after_getpc.size() + am_back) {
			instructions_after_getpc.push_back(inst);
		}
		memset(regs_target,false,RegistersCount);
		int kol = 0;
		for (int i = 0; i < amount; i++) {
			if (a[i]==num) {
				kol++;
			}
		}
		if (kol >= 2) {
			int neednum = num;
			for (barrier = 0; barrier < strnum + 10; barrier++) { /// TODO: why 10?
				cycle[barrier] = Command(num,inst);
				if (!emulator->get_command(buff)) {
					LOG << " Execution error, stopping instance." << endl;
					Timer::stop(TimeLaunches);
					return;
				}
				num = emulator->get_register(EIP);
				get_instruction(&inst, (BYTE *) buff, mode);
				LOG << "  Command: 0x" << hex << num << ": " << instruction_string(&inst, num) << endl;
				if (!emulator->step()) {
					LOG << " Execution error, stopping instance." << endl;
					Timer::stop(TimeLaunches);
					return;
				}
				if (num==neednum) {
					flag = true;
					break;
				}
			}
		}
		if (flag) {
			break;
		}
		if (is_write_indirect(&inst)) {
			a[amount++] = num;
		}
	}
	
	if (flag) {
		k = verify(cycle, barrier+1);

		if (log) {
			LOG << " Cycle found: " << endl;
			for (uint i = 0; i <= barrier; i++) {
				LOG << " 0x" << hex << cycle[i].addr << ":  " << instruction_string(&(cycle[i].inst), cycle[i].addr) << endl;
			}
			if (k != -1) {
				LOG << " Indirect write in line #" << k << ", launched from position 0x" << hex << pos << endl;
			} else {
				LOG << " No indirect writes." << endl;
			}
		}

		if (k != -1) {
			matches++;
			cout << "Instruction \"" << instruction_string(pos_getpc) << "\" on position 0x" << hex << pos_getpc << "." << endl;
			cout << "Cycle found: " << endl;
			for (uint i = 0; i <= barrier; i++) {
				cout << " 0x" << hex << cycle[i].addr << ":  " << instruction_string(&(cycle[i].inst), cycle[i].addr) << endl;
			}
			cout << " Indirect write in line #" << k << ", launched from position 0x" << hex << pos << endl;
#ifdef FINDER_ONCE
			Timer::stop(TimeLaunches);
			exit(0);
#endif
		}
	}
	Timer::stop(TimeLaunches);
}

int FinderCycle::find() {
	matches = 0;
	Timer::start(TimeFind);
	INSTRUCTION inst;
	for (uint i=reader->start(); i<reader->size(); i++) {
		/// TODO: check opcodes
		switch (reader->pointer()[i]) {
			/// fsave/fnsave: 0x9bdd, 0xdd
			case 0x9b:
				if ((reader->pointer()[i+1]) != 0xdd) { /// TODO: check if i+1 is present
					continue;
				}
			case 0xdd:
				break;
			/// fstenv/fnstenv: 0xf2d9, 0xd9
			case 0xf2:
				if ((reader->pointer()[i+1]) != 0xd9) { /// TODO: check if i+1 is present
					continue;
				}
			case 0xd9:
				break;
			/// call: 0xe8, 0xff, 0x9a
			case 0xe8:
			case 0xff:
			case 0x9a:
				break;
			default:
				continue;
		}
		uint len = instruction(&inst, i);
		if (!len || (len + i > reader->size())) {
			continue;
		}
		switch (inst.type) {
			case INSTRUCTION_TYPE_FPU_CTRL:
				if (	(strcmp(inst.ptr->mnemonic,"fstenv") == 0) ||
					(strcmp(inst.ptr->mnemonic,"fsave") == 0)) {
					break;
				}
				continue;
			case INSTRUCTION_TYPE_CALL:
				if (	(strcmp(inst.ptr->mnemonic,"call") == 0) &&
					(inst.op1.type == OPERAND_TYPE_IMMEDIATE)) {
					break;
				}
				continue;
			default:
				continue;
		}
		//cerr << "0x" << hex << i << ": 0x" << hex << (int) reader->pointer()[i] << " | 0x" << hex << (int) inst.opcode << " " << inst.ptr->mnemonic << endl;
		pos_getpc = i;
		LOG << "Instruction \"" << instruction_string(i) << "\" on position 0x" << hex << i << "." << endl;
		find_memory_and_jump(i);
		//find_jump(i);
		LOG << "*********************************************************" << endl;
		instructions_after_getpc.clear();
	}
	Timer::stop(TimeFind);
	return matches;
}

void FinderCycle::find_memory_and_jump(int pos)
{
	Timer::start(TimeFindMemoryAndJump);
	INSTRUCTION inst;
	uint len;
	set<uint> nofollow;
	for (uint p = pos, count_instructions = 0; p < reader->size() && count_instructions < maxForward; p += len, count_instructions++) {
		len = instruction(&inst,p);
		if (!len || (len + p > reader->size())) {
			LOG <<  " Dissasembling failed." << endl;
			Timer::stop(TimeFindMemoryAndJump);
			return;
		}
		LOG << "Instruction: " << instruction_string(&inst,p) << " on position 0x" << hex << p << endl;
		instructions_after_getpc.push_back(inst);
		switch (inst.type) {
			/// TODO: check
			case INSTRUCTION_TYPE_JMP:
			case INSTRUCTION_TYPE_JMPC:
				if (	(inst.op1.type==OPERAND_TYPE_MEMORY) &&
					(inst.op1.basereg!=REG_NOP)) {
					LOG << " Indirect jump detected: " << instruction_string(&inst) << " on position 0x" << hex << p << endl;
					get_operands(&inst);
				}
				if (	(!nofollow.count(p)) &&
					(strcmp(inst.ptr->mnemonic,"jmp")==0) &&
					(inst.op1.type==OPERAND_TYPE_IMMEDIATE)) {
					nofollow.insert(p);
					p += inst.op1.immediate;
					continue;
				}
				break;
			case INSTRUCTION_TYPE_CALL:
				if (	(!nofollow.count(p)) &&
					(strcmp(inst.ptr->mnemonic,"call")==0) &&
					(inst.op1.type==OPERAND_TYPE_IMMEDIATE)) {
					nofollow.insert(p);
					p += inst.op1.immediate;
					continue;
				}
				break;
			default:;
		}
		if (!is_write_indirect(&inst)) {
			continue;
		}
		LOG << "Write to memory detected: " << instruction_string(&inst,p) << " on position 0x" << hex << p << endl;
		if (start_positions.count(p)) {
			LOG << "Not running, already checked." << endl;
			Timer::stop(TimeFindMemoryAndJump);
			return;
		}
		start_positions.insert(p);
		memset(regs_known,false,RegistersCount);
		memset(regs_target,false,RegistersCount);
		get_operands(&inst);
		check(&instructions_after_getpc);
		int em_start = backwards_traversal(pos_getpc);
		if (em_start < 0) {
			LOG <<  " Backwards traversal failed (nothing suitable found)." << endl;
			Timer::stop(TimeFindMemoryAndJump);
			return;
		}
		print_commands(&instructions_after_getpc,1);
		launch(em_start);
		Timer::stop(TimeFindMemoryAndJump);
		return;
	}
	Timer::stop(TimeFindMemoryAndJump);
}

bool FinderCycle::regs_closed() {
	for (int i=0; i<RegistersCount; i++) {
		if (regs_target[i]) {
			return false;
		}
	}
	return true;
}
void FinderCycle::check(vector <INSTRUCTION>* instructions)
{
	for (int k=instructions->size()-1;k>=0;k--) {
		check(&((*instructions)[k]));
	}
}
void FinderCycle::add_target(OPERAND *op) {
	if (MASK_FLAGS(op->flags)==F_f) {
		return;
	}
	switch (op->type) {
		case OPERAND_TYPE_REGISTER:
			if (op->reg == REG_ESP) {
				break;
			}
			regs_target[int_to_reg(op->reg)] = true;
			break;
		case OPERAND_TYPE_MEMORY:
			if (op->basereg == REG_NOP || op->reg == REG_ESP) {
				break;
			}
			regs_target[int_to_reg(op->basereg)] = true;
			break;
		default:;
	}
}
void FinderCycle::get_operands(INSTRUCTION *inst)
{
	if (inst->type == INSTRUCTION_TYPE_LODS) {
		regs_target[ESI] = true;
	}
	if (inst->type == INSTRUCTION_TYPE_LOOP) {
		regs_target[ECX] = true;
	}
	if (inst->op1.type==OPERAND_TYPE_MEMORY) {
		add_target(&(inst->op1));
	}
	add_target(&(inst->op2));
	add_target(&(inst->op3));
}
void FinderCycle::check(INSTRUCTION *inst)
{
	/// TODO: Add more instruction types here.
	/// TODO: WARNING: Check logic.
	int r;
	switch (inst->type)
	{
		case INSTRUCTION_TYPE_LODS:
			regs_known[EAX] = true;
			regs_target[EAX] = false;
			regs_target[ESI] = true;
			break;
		case INSTRUCTION_TYPE_STOS:
			regs_target[EAX] = true;
			regs_target[EDI] = true;
			break;
		case INSTRUCTION_TYPE_XOR:
		case INSTRUCTION_TYPE_SUB:
		case INSTRUCTION_TYPE_SBB:
		case INSTRUCTION_TYPE_DIV:
		case INSTRUCTION_TYPE_IDIV:
			if (inst->op1.type != OPERAND_TYPE_REGISTER) {
				break;
			}
			r = int_to_reg(inst->op1.reg);
			if ((inst->op1.type == inst->op2.type) && (inst->op1.reg == inst->op2.reg)) {
				regs_target[r] = false;
				regs_known[r] = true;
				break;
			}
			if (regs_target[r]) {
				add_target(&(inst->op2));
			}
			break;
		case INSTRUCTION_TYPE_ADD:
		case INSTRUCTION_TYPE_AND:
		case INSTRUCTION_TYPE_OR:
		case INSTRUCTION_TYPE_MUL:
		case INSTRUCTION_TYPE_IMUL:
			if (inst->op1.type != OPERAND_TYPE_REGISTER) {
				break;
			}
			r = int_to_reg(inst->op1.reg);
			if (regs_target[r]) {
				add_target(&(inst->op2));
			}
			break;
		case INSTRUCTION_TYPE_MOV:
		case INSTRUCTION_TYPE_LEA:
			if (inst->op1.type != OPERAND_TYPE_REGISTER) {
				break;
			}
			r = int_to_reg(inst->op1.reg);
			regs_known[r] = true;
			if (regs_target[r]) {
				regs_target[r] = false;
				add_target(&(inst->op2));
			}
			break;
		case INSTRUCTION_TYPE_POP:
			if (inst->op1.type != OPERAND_TYPE_REGISTER) {
				break;
			}
			r = int_to_reg(inst->op1.reg);
			regs_known[r] = true;
			regs_target[r] = false;
			regs_target[ESP] = true;
			break;
		case INSTRUCTION_TYPE_PUSH: /// TODO: check operands
			regs_target[ESP] = false;
			break;
		case INSTRUCTION_TYPE_CALL:
			regs_target[ESP] = false;
			regs_known[ESP] = true;
			//if (get_write_indirect(inst,&r)) {
			//	regs_target[r]=true;
			//	regs_known[r]=false;
			//}
			//if (inst->op1.type==OPERAND_TYPE_REGISTER) {
			//	r = int_to_reg(inst->op1.reg);
			//	regs_target[r]=true;
			//	regs_known[r]=false;
			//}
			break;
		case INSTRUCTION_TYPE_OTHER:
			if (strcmp(inst->ptr->mnemonic,"cpuid")==0) {
				regs_target[EAX] = true;
				regs_target[EBX] = false;
				regs_target[ECX] = false;
				regs_target[EDX] = false;
				regs_known[EAX] = false;
				regs_known[EBX] = true;
				regs_known[ECX] = true;
				regs_known[EDX] = true;
			}
			break;
		case INSTRUCTION_TYPE_FPU_CTRL:
			if (strcmp(inst->ptr->mnemonic,"fstenv")==0) {
				add_target(&(inst->op1));
				if (regs_target[ESP]) { // If we need ESP. Else, use general fpu instuction logic.
					regs_target[ESP] = false;
					regs_known[ESP] = true;
					regs_target[HASFPU] = true;
					break;
				}
			} // No break here, going to default processing.
		default:
			if (MASK_EXT(inst->flags) == EXT_CP) { // Co-processor: FPU instructions
				regs_target[HASFPU] = false;
				regs_known[HASFPU] = true;
			};
			break;
	}
}

int FinderCycle::backwards_traversal(int pos)
{
	if (regs_closed()) {
		return pos;
	}
	Timer::start(TimeBackwardsTraversal);
	bool regs_target_bak[RegistersCount], regs_known_bak[RegistersCount];
	memcpy(regs_target_bak,regs_target,RegistersCount);
	memcpy(regs_known_bak,regs_known,RegistersCount);
	vector <int> queue[2];
	map<int,INSTRUCTION> instructions;
	INSTRUCTION inst;
	queue[0].push_back(pos);
	int m = 0;
	vector <INSTRUCTION> commands;
	for (uint n = 0; n < maxBackward; n++) {
		queue[m^1].clear();
		for (vector<int>::iterator p=queue[m].begin(); p!=queue[m].end(); p++) {
			for (int i=1; (i<=MaxCommandSize) && (i<=(*p)); i++) {
				int curr = (*p) - i;
				bool ok = false;
				int len = instruction(&inst,curr);
				switch (inst.type) {
					case INSTRUCTION_TYPE_JMP:
					case INSTRUCTION_TYPE_JMPC:
					case INSTRUCTION_TYPE_JECXZ:
						ok = (i == (inst.op1.immediate + len));
						break;
					default:;
				}
				if (len!=i && !ok) {
					continue;
				}
				instructions[curr] = inst;
				queue[m^1].push_back((*p)-i);
				commands.clear();
				for (uint j=curr,k=0; k<=n; k++) {
					commands.push_back(instructions[j]);
					j += instructions[j].length;
				}
				check(&commands);
				am_back = commands.size();
				bool ret = regs_closed();
/*				if (log) {
					LOG << "   BACKWARDS TRAVERSAL ITERATION" << endl;
					print_commands(&commands, 0);
					if (ret) {
						LOG << "Backwards traversal iteration succeeded." << endl;
					} else {
						LOG << "Backwards traversal iteration failed. Unknown registers: ";
						for (int i=0; i<RegistersCount; i++) {
							if (regs_target[i]) {
								LOG << " " << dec << i;
							}
						}
						LOG << endl;
					}
				}*/
				memcpy(regs_target,regs_target_bak,RegistersCount);
				memcpy(regs_known,regs_known_bak,RegistersCount);
				if (ret) {
					Timer::stop(TimeBackwardsTraversal);
					return curr;
				}
			}
		}
		m ^= 1;
		/// TODO: We should also check all static jumps to this point.
	}
	Timer::stop(TimeBackwardsTraversal);
	return -1;
}
int FinderCycle::verify(Command *cycle, int size)
{
	for (int i=0;i<size;i++) {
		if (is_write_indirect(&(cycle[i].inst))) {
			if (verify_changing_reg(&(cycle[i].inst), cycle, size)) {
				return i+1;
			}
		}
	}
	return -1;
}

bool FinderCycle::verify_changing_reg(INSTRUCTION *inst, Command *cycle, int size)
{
	int	mem  = inst->op1.displacement,
		reg0 = inst->op1.basereg,
		reg1 = inst->op1.reg,
		reg2 = inst->op1.indexreg;
	if (reg0 != REG_NOP) {
		mem += emulator->get_register((Register) int_to_reg(reg0));
	}
	if (reg1 != REG_NOP) {
		mem += emulator->get_register((Register) int_to_reg(reg1));
	}
	if (reg2 != REG_NOP) {
		mem += emulator->get_register((Register) int_to_reg(reg2));
	}
	if (inst->type == INSTRUCTION_TYPE_STOS) {
		reg0 = REG_EDI;
		mem = emulator->get_register((Register) int_to_reg(reg0));
	}
	if ((mem==0) || !reader->is_within_one_block(mem,cycle[0].addr)) {
		return false;
	}
	for (int i=0;i<size;i++) {
		if (	is_write(&(cycle[i].inst)) && 
			(cycle[i].inst.op1.type == OPERAND_TYPE_REGISTER) && 
			(	((reg0 != REG_NOP) && (reg0 == cycle[i].inst.op1.reg)) ||
				((reg1 != REG_NOP) && (reg1 == cycle[i].inst.op1.reg)) ||
				((reg2 != REG_NOP) && (reg2 == cycle[i].inst.op1.reg))
			)) {
			return true;
		}
		switch (cycle[i].inst.type) {
			case INSTRUCTION_TYPE_LOOP:
				if ((reg0 == REG_ECX) || (reg1 == REG_ECX) || (reg2 == REG_ECX)) {
					return true;
				}
				break;
			case INSTRUCTION_TYPE_LODS:
				if ((reg0 == REG_ESI) || (reg1 == REG_ESI) || (reg2 == REG_ESI)) {
					return true;
				}
				break;
			case INSTRUCTION_TYPE_STOS:
				if ((reg0 == REG_EDI) || (reg1 == REG_EDI) || (reg2 == REG_EDI)) {
					return true;
				}
				break;
			default:;
		}
	}
	return false;
}
