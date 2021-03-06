/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Copyright (C) 2012  Clifford Wolf <clifford@clifford.at>
 *  
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *  
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include "kernel/log.h"
#include "kernel/register.h"
#include "kernel/sigtools.h"
#include "kernel/consteval.h"
#include "kernel/celltypes.h"
#include "fsmdata.h"
#include <string.h>

struct FsmOpt
{
	FsmData fsm_data;
	RTLIL::Cell *cell;
	RTLIL::Module *module;
	
	bool signal_is_unused(RTLIL::SigSpec sig)
	{
		assert(sig.width == 1);
		sig.optimize();

		RTLIL::Wire *wire = sig.chunks[0].wire;
		int bit = sig.chunks[0].offset;

		if (!wire || wire->attributes.count("\\unused_bits") == 0)
			return false;
		
		char *str = strdup(wire->attributes["\\unused_bits"].decode_string().c_str());
		for (char *tok = strtok(str, " "); tok != NULL; tok = strtok(NULL, " ")) {
			if (tok[0] && bit == atoi(tok))
				return true;
		}
		free(str);

		return false;
	}

	void opt_const_and_unused_inputs()
	{
		RTLIL::SigSpec ctrl_in = cell->connections["\\CTRL_IN"];
		std::vector<bool> ctrl_in_used(ctrl_in.width);

		std::vector<FsmData::transition_t> new_transition_table;
		for (auto &tr : fsm_data.transition_table) {
			for (int i = 0; i < ctrl_in.width; i++) {
				RTLIL::SigSpec ctrl_bit = ctrl_in.extract(i, 1);
				if (ctrl_bit.is_fully_const()) {
					if (tr.ctrl_in.bits[i] <= RTLIL::State::S1 && RTLIL::SigSpec(tr.ctrl_in.bits[i]) != ctrl_bit)
						goto delete_this_transition;
					continue;
				}
				if (tr.ctrl_in.bits[i] <= RTLIL::State::S1)
					ctrl_in_used[i] = true;
			}
			new_transition_table.push_back(tr);
		delete_this_transition:;
		}

		for (int i = int(ctrl_in_used.size())-1; i >= 0; i--) {
			if (!ctrl_in_used[i]) {
				log("  Removing unused input signal %s.\n", log_signal(cell->connections["\\CTRL_IN"].extract(i, 1)));
				for (auto &tr : new_transition_table) {
					RTLIL::SigSpec tmp(tr.ctrl_in);
					tmp.remove(i, 1);
					tr.ctrl_in = tmp.as_const();
				}
				cell->connections["\\CTRL_IN"].remove(i, 1);
				fsm_data.num_inputs--;
			}
		}

		fsm_data.transition_table.swap(new_transition_table);
		new_transition_table.clear();
	}

	void opt_unused_outputs()
	{
		for (int i = 0; i < fsm_data.num_outputs; i++) {
			RTLIL::SigSpec sig = cell->connections["\\CTRL_OUT"].extract(i, 1);
			if (signal_is_unused(sig)) {
				log("  Removing unused output signal %s.\n", log_signal(sig));
				cell->connections["\\CTRL_OUT"].remove(i, 1);
				for (auto &tr : fsm_data.transition_table) {
					RTLIL::SigSpec tmp(tr.ctrl_out);
					tmp.remove(i, 1);
					tr.ctrl_out = tmp.as_const();
				}
				fsm_data.num_outputs--;
				i--;
			}
		}
	}

	void opt_alias_inputs()
	{
		RTLIL::SigSpec &ctrl_in = cell->connections["\\CTRL_IN"];

		for (int i = 0; i < ctrl_in.width; i++)
		for (int j = i+1; j < ctrl_in.width; j++)
			if (ctrl_in.extract(i, 1) == ctrl_in.extract(j, 1))
			{
				log("  Optimize handling of signal %s that is connected to inputs %d and %d.\n", log_signal(ctrl_in.extract(i, 1)), i, j);
				std::vector<FsmData::transition_t> new_transition_table;

				for (auto tr : fsm_data.transition_table)
				{
					RTLIL::State &si = tr.ctrl_in.bits[i];
					RTLIL::State &sj = tr.ctrl_in.bits[j];

					if (si > RTLIL::State::S1)
						si = sj;
					else if (sj > RTLIL::State::S1)
						sj = si;

					if (si == sj) {
						RTLIL::SigSpec tmp(tr.ctrl_in);
						tmp.remove(j, 1);
						tr.ctrl_in = tmp.as_const();
						new_transition_table.push_back(tr);
					}
				}

				ctrl_in.remove(j--, 1);
				fsm_data.num_inputs--;

				fsm_data.transition_table.swap(new_transition_table);
				new_transition_table.clear();
			}
	}

	void opt_feedback_inputs()
	{
		RTLIL::SigSpec &ctrl_in = cell->connections["\\CTRL_IN"];
		RTLIL::SigSpec &ctrl_out = cell->connections["\\CTRL_OUT"];

		for (int j = 0; j < ctrl_out.width; j++)
		for (int i = 0; i < ctrl_in.width; i++)
			if (ctrl_in.extract(i, 1) == ctrl_out.extract(j, 1))
			{
				log("  Optimize handling of signal %s that is connected to input %d and output %d.\n", log_signal(ctrl_in.extract(i, 1)), i, j);
				std::vector<FsmData::transition_t> new_transition_table;

				for (auto tr : fsm_data.transition_table)
				{
					RTLIL::State &si = tr.ctrl_in.bits[i];
					RTLIL::State &sj = tr.ctrl_out.bits[j];

					if (si > RTLIL::State::S1 || si == sj) {
						RTLIL::SigSpec tmp(tr.ctrl_in);
						tmp.remove(i, 1);
						tr.ctrl_in = tmp.as_const();
						new_transition_table.push_back(tr);
					}
				}

				ctrl_in.remove(i--, 1);
				fsm_data.num_inputs--;

				fsm_data.transition_table.swap(new_transition_table);
				new_transition_table.clear();
			}
	}

	void opt_find_dont_care_worker(std::set<RTLIL::Const> &set, int bit, FsmData::transition_t &tr, bool &did_something)
	{
		std::set<RTLIL::Const> new_set;

		for (auto &pattern : set)
		{
			if (pattern.bits[bit] > RTLIL::State::S1) {
				new_set.insert(pattern);
				continue;
			}

			RTLIL::Const other_pattern = pattern;

			if (pattern.bits[bit] == RTLIL::State::S1)
				other_pattern.bits[bit] = RTLIL::State::S0;
			else
				other_pattern.bits[bit] = RTLIL::State::S1;

			if (set.count(other_pattern) > 0) {
				log("  Merging pattern %s and %s from group (%d %d %s).\n", log_signal(pattern), log_signal(other_pattern),
						tr.state_in, tr.state_out, log_signal(tr.ctrl_out));
				other_pattern.bits[bit] = RTLIL::State::Sa;
				new_set.insert(other_pattern);
				did_something = true;
				continue;
			}

			new_set.insert(pattern);
		}

		set.swap(new_set);
	}

	void opt_find_dont_care()
	{
		typedef std::pair<std::pair<int, int>, RTLIL::Const> group_t;
		std::map<group_t, std::set<RTLIL::Const>> transitions_by_group;

		for (auto &tr : fsm_data.transition_table) {
			group_t group(std::pair<int, int>(tr.state_in, tr.state_out), tr.ctrl_out);
			transitions_by_group[group].insert(tr.ctrl_in);
		}

		fsm_data.transition_table.clear();
		for (auto &it : transitions_by_group)
		{
			FsmData::transition_t tr;
			tr.state_in = it.first.first.first;
			tr.state_out = it.first.first.second;
			tr.ctrl_out = it.first.second;

			bool did_something = true;
			while (did_something) {
				did_something = false;
				for (int i = 0; i < fsm_data.num_inputs; i++)
					opt_find_dont_care_worker(it.second, i, tr, did_something);
			}

			for (auto &ci : it.second) {
				tr.ctrl_in = ci;
				fsm_data.transition_table.push_back(tr);
			}
		}
	}

	FsmOpt(RTLIL::Cell *cell, RTLIL::Module *module)
	{
		log("Optimizing FSM `%s' from module `%s'.\n", cell->name.c_str(), module->name.c_str());

		fsm_data.copy_from_cell(cell);
		this->cell = cell;
		this->module = module;

		opt_unused_outputs();

		opt_alias_inputs();
		opt_feedback_inputs();
		opt_find_dont_care();

		opt_const_and_unused_inputs();

		fsm_data.copy_to_cell(cell);
	}
};

void FsmData::optimize_fsm(RTLIL::Cell *cell, RTLIL::Module *module)
{
	FsmOpt fsmopt(cell, module);
}

struct FsmOptPass : public Pass {
	FsmOptPass() : Pass("fsm_opt", "optimize finite state machines") { }
	virtual void help()
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    fsm_opt [selection]\n");
		log("\n");
		log("This pass optimizes FSM cells. It detects which output signals are actually\n");
		log("not used and removes them from the FSM. This pass is usually used in\n");
		log("combination with the 'opt_clean' pass (see also 'help fsm').\n");
		log("\n");
	}
	virtual void execute(std::vector<std::string> args, RTLIL::Design *design)
	{
		log_header("Executing FSM_OPT pass (simple optimizations of FSMs).\n");
		extra_args(args, 1, design);

		for (auto &mod_it : design->modules) {
			if (design->selected(mod_it.second))
				for (auto &cell_it : mod_it.second->cells)
					if (cell_it.second->type == "$fsm" and design->selected(mod_it.second, cell_it.second))
						FsmData::optimize_fsm(cell_it.second, mod_it.second);
		}
	}
} FsmOptPass;
 
