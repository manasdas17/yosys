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

#include "kernel/register.h"
#include "kernel/sigtools.h"
#include "kernel/log.h"
#include <stdlib.h>
#include <stdio.h>

static void proc_get_const(RTLIL::SigSpec &sig, RTLIL::CaseRule &rule)
{
	assert(rule.compare.size() == 0);

	while (1) {
		sig.optimize();
		RTLIL::SigSpec tmp = sig;
		for (auto &it : rule.actions)
			tmp.replace(it.first, it.second);
		if (tmp == sig)
			break;
		sig = tmp;
	}
}

static void proc_init(RTLIL::Module *mod, RTLIL::Process *proc)
{
	bool found_init = false;

	for (auto &sync : proc->syncs)
		if (sync->type == RTLIL::SyncType::STi)
		{
			found_init = true;
			log("Found init rule in `%s.%s'.\n", mod->name.c_str(), proc->name.c_str());

			for (auto &action : sync->actions)
			{
				RTLIL::SigSpec lhs = action.first;
				RTLIL::SigSpec rhs = action.second;

				lhs.optimize();
				proc_get_const(rhs, proc->root_case);

				if (!rhs.is_fully_const())
					log_cmd_error("Failed to get a constant init value for %s: %s\n", log_signal(lhs), log_signal(rhs));

				int offset = 0;
				for (size_t i = 0; i < lhs.chunks.size(); i++) {
					if (lhs.chunks[i].wire == NULL)
						continue;
					RTLIL::Wire *wire = lhs.chunks[i].wire;
					RTLIL::SigSpec value = rhs.extract(offset, lhs.chunks[i].width);
					if (value.width != wire->width)
						log_cmd_error("Init value is not for the entire wire: %s = %s\n", log_signal(lhs.chunks[i]), log_signal(value));
					log("  Setting init value: %s = %s\n", log_signal(wire), log_signal(value));
					wire->attributes["\\init"] = value.as_const();
					offset += wire->width;
				}
			}
		}

	if (found_init) {
		std::vector<RTLIL::SyncRule*> new_syncs;
		for (auto &sync : proc->syncs)
			if (sync->type == RTLIL::SyncType::STi)
				delete sync;
			else
				new_syncs.push_back(sync);
		proc->syncs.swap(new_syncs);
	}
}

struct ProcInitPass : public Pass {
	ProcInitPass() : Pass("proc_init", "convert initial block to init attributes") { }
	virtual void help()
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    proc_init [selection]\n");
		log("\n");
		log("This pass extracts the 'init' actions from processes (generated from verilog\n");
		log("'initial' blocks) and sets the initial value to the 'init' attribute on the\n");
		log("respective wire.\n");
		log("\n");
	}
	virtual void execute(std::vector<std::string> args, RTLIL::Design *design)
	{
		log_header("Executing PROC_INIT pass (extract init attributes).\n");

		extra_args(args, 1, design);

		for (auto &mod_it : design->modules)
			if (design->selected(mod_it.second))
				for (auto &proc_it : mod_it.second->processes)
					if (design->selected(mod_it.second, proc_it.second))
						proc_init(mod_it.second, proc_it.second);
	}
} ProcInitPass;
 
