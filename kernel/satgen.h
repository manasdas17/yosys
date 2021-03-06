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

#ifndef SATGEN_H
#define SATGEN_H

#include "kernel/rtlil.h"
#include "kernel/sigtools.h"
#include "kernel/celltypes.h"

#include "libs/ezsat/ezminisat.h"
typedef ezMiniSAT ezDefaultSAT;

struct SatGen
{
	ezSAT *ez;
	SigMap *sigmap;
	std::string prefix;
	SigPool initial_state;
	std::map<std::string, RTLIL::SigSpec> asserts_a, asserts_en;
	bool ignore_div_by_zero;
	bool model_undef;

	SatGen(ezSAT *ez, SigMap *sigmap, std::string prefix = std::string()) :
			ez(ez), sigmap(sigmap), prefix(prefix), ignore_div_by_zero(false), model_undef(false)
	{
	}

	void setContext(SigMap *sigmap, std::string prefix = std::string())
	{
		this->sigmap = sigmap;
		this->prefix = prefix;
	}

	std::vector<int> importSigSpecWorker(RTLIL::SigSpec &sig, std::string &pf, bool undef_mode, bool dup_undef)
	{
		log_assert(!undef_mode || model_undef);
		sigmap->apply(sig);
		sig.expand();

		std::vector<int> vec;
		vec.reserve(sig.chunks.size());

		for (auto &c : sig.chunks)
			if (c.wire == NULL) {
				RTLIL::State bit = c.data.bits.at(0);
				if (model_undef && dup_undef && bit == RTLIL::State::Sx)
					vec.push_back(ez->frozen_literal());
				else
					vec.push_back(bit == (undef_mode ? RTLIL::State::Sx : RTLIL::State::S1) ? ez->TRUE : ez->FALSE);
			} else {
				std::string name = pf + stringf(c.wire->width == 1 ?  "%s" : "%s [%d]", RTLIL::id2cstr(c.wire->name), c.offset);
				vec.push_back(ez->frozen_literal(name));
			}
		return vec;
	}

	std::vector<int> importSigSpec(RTLIL::SigSpec sig, int timestep = -1)
	{
		log_assert(timestep != 0);
		std::string pf = prefix + (timestep == -1 ? "" : stringf("@%d:", timestep));
		return importSigSpecWorker(sig, pf, false, false);
	}

	std::vector<int> importDefSigSpec(RTLIL::SigSpec sig, int timestep = -1)
	{
		log_assert(timestep != 0);
		std::string pf = prefix + (timestep == -1 ? "" : stringf("@%d:", timestep));
		return importSigSpecWorker(sig, pf, false, true);
	}

	std::vector<int> importUndefSigSpec(RTLIL::SigSpec sig, int timestep = -1)
	{
		log_assert(timestep != 0);
		std::string pf = "undef:" + prefix + (timestep == -1 ? "" : stringf("@%d:", timestep));
		return importSigSpecWorker(sig, pf, true, false);
	}

	void getAsserts(RTLIL::SigSpec &sig_a, RTLIL::SigSpec &sig_en, int timestep = -1)
	{
		std::string pf = prefix + (timestep == -1 ? "" : stringf("@%d:", timestep));
		sig_a = asserts_a[pf];
		sig_en = asserts_en[pf];
	}

	int importAsserts(int timestep = -1)
	{
		std::vector<int> check_bits, enable_bits;
		std::string pf = prefix + (timestep == -1 ? "" : stringf("@%d:", timestep));
		if (model_undef) {
			check_bits = ez->vec_and(ez->vec_not(importUndefSigSpec(asserts_a[pf], timestep)), importDefSigSpec(asserts_a[pf], timestep));
			enable_bits = ez->vec_and(ez->vec_not(importUndefSigSpec(asserts_en[pf], timestep)), importDefSigSpec(asserts_en[pf], timestep));
		} else {
			check_bits = importDefSigSpec(asserts_a[pf], timestep);
			enable_bits = importDefSigSpec(asserts_en[pf], timestep);
		}
		return ez->vec_reduce_and(ez->vec_or(check_bits, ez->vec_not(enable_bits)));
	}

	int signals_eq(RTLIL::SigSpec lhs, RTLIL::SigSpec rhs, int timestep_lhs = -1, int timestep_rhs = -1)
	{
		if (timestep_rhs < 0)
			timestep_rhs = timestep_lhs;

		assert(lhs.width == rhs.width);

		std::vector<int> vec_lhs = importSigSpec(lhs, timestep_lhs);
		std::vector<int> vec_rhs = importSigSpec(rhs, timestep_rhs);

		if (!model_undef)
			return ez->vec_eq(vec_lhs, vec_rhs);

		std::vector<int> undef_lhs = importUndefSigSpec(lhs, timestep_lhs);
		std::vector<int> undef_rhs = importUndefSigSpec(rhs, timestep_rhs);

		std::vector<int> eq_bits;
		for (int i = 0; i < lhs.width; i++)
			eq_bits.push_back(ez->AND(ez->IFF(undef_lhs.at(i), undef_rhs.at(i)),
					ez->IFF(ez->OR(vec_lhs.at(i), undef_lhs.at(i)), ez->OR(vec_rhs.at(i), undef_rhs.at(i)))));
		return ez->expression(ezSAT::OpAnd, eq_bits);
	}

	void extendSignalWidth(std::vector<int> &vec_a, std::vector<int> &vec_b, RTLIL::Cell *cell, size_t y_width = 0, bool forced_signed = false)
	{
		bool is_signed = forced_signed;
		if (!forced_signed && cell->parameters.count("\\A_SIGNED") > 0 && cell->parameters.count("\\B_SIGNED") > 0)
			is_signed = cell->parameters["\\A_SIGNED"].as_bool() && cell->parameters["\\B_SIGNED"].as_bool();
		while (vec_a.size() < vec_b.size() || vec_a.size() < y_width)
			vec_a.push_back(is_signed && vec_a.size() > 0 ? vec_a.back() : ez->FALSE);
		while (vec_b.size() < vec_a.size() || vec_b.size() < y_width)
			vec_b.push_back(is_signed && vec_b.size() > 0 ? vec_b.back() : ez->FALSE);
	}

	void extendSignalWidth(std::vector<int> &vec_a, std::vector<int> &vec_b, std::vector<int> &vec_y, RTLIL::Cell *cell, bool forced_signed = false)
	{
		extendSignalWidth(vec_a, vec_b, cell, vec_y.size(), forced_signed);
		while (vec_y.size() < vec_a.size())
			vec_y.push_back(ez->literal());
	}

	void extendSignalWidthUnary(std::vector<int> &vec_a, std::vector<int> &vec_y, RTLIL::Cell *cell, bool forced_signed = false)
	{
		bool is_signed = forced_signed || (cell->parameters.count("\\A_SIGNED") > 0 && cell->parameters["\\A_SIGNED"].as_bool());
		while (vec_a.size() < vec_y.size())
			vec_a.push_back(is_signed && vec_a.size() > 0 ? vec_a.back() : ez->FALSE);
		while (vec_y.size() < vec_a.size())
			vec_y.push_back(ez->literal());
	}

	void undefGating(std::vector<int> &vec_y, std::vector<int> &vec_yy, std::vector<int> &vec_undef)
	{
		assert(model_undef);
		ez->assume(ez->expression(ezSAT::OpAnd, ez->vec_or(vec_undef, ez->vec_iff(vec_y, vec_yy))));
	}

	bool importCell(RTLIL::Cell *cell, int timestep = -1)
	{
		bool arith_undef_handled = false;
		bool is_arith_compare = cell->type == "$lt" || cell->type == "$le" || cell->type == "$ge" || cell->type == "$gt";

		if (model_undef && (cell->type == "$add" || cell->type == "$sub" || cell->type == "$mul" || cell->type == "$div" || cell->type == "$mod" || is_arith_compare))
		{
			std::vector<int> undef_a = importUndefSigSpec(cell->connections.at("\\A"), timestep);
			std::vector<int> undef_b = importUndefSigSpec(cell->connections.at("\\B"), timestep);
			std::vector<int> undef_y = importUndefSigSpec(cell->connections.at("\\Y"), timestep);
			if (is_arith_compare)
				extendSignalWidth(undef_a, undef_b, cell, true);
			else
				extendSignalWidth(undef_a, undef_b, undef_y, cell, true);

			int undef_any_a = ez->expression(ezSAT::OpOr, undef_a);
			int undef_any_b = ez->expression(ezSAT::OpOr, undef_b);
			int undef_y_bit = ez->OR(undef_any_a, undef_any_b);

			if (cell->type == "$div" || cell->type == "$mod") {
				std::vector<int> b = importSigSpec(cell->connections.at("\\B"), timestep);
				undef_y_bit = ez->OR(undef_y_bit, ez->NOT(ez->expression(ezSAT::OpOr, b)));
			}

			if (is_arith_compare) {
				for (size_t i = 1; i < undef_y.size(); i++)
					ez->SET(ez->FALSE, undef_y.at(i));
				ez->SET(undef_y_bit, undef_y.at(0));
			} else {
				std::vector<int> undef_y_bits(undef_y.size(), undef_y_bit);
				ez->assume(ez->vec_eq(undef_y_bits, undef_y));
			}

			arith_undef_handled = true;
		}

		if (cell->type == "$_AND_" || cell->type == "$_OR_" || cell->type == "$_XOR_" ||
				cell->type == "$and" || cell->type == "$or" || cell->type == "$xor" || cell->type == "$xnor" ||
				cell->type == "$add" || cell->type == "$sub")
		{
			std::vector<int> a = importDefSigSpec(cell->connections.at("\\A"), timestep);
			std::vector<int> b = importDefSigSpec(cell->connections.at("\\B"), timestep);
			std::vector<int> y = importDefSigSpec(cell->connections.at("\\Y"), timestep);
			extendSignalWidth(a, b, y, cell);

			std::vector<int> yy = model_undef ? ez->vec_var(y.size()) : y;

			if (cell->type == "$and" || cell->type == "$_AND_")
				ez->assume(ez->vec_eq(ez->vec_and(a, b), yy));
			if (cell->type == "$or" || cell->type == "$_OR_")
				ez->assume(ez->vec_eq(ez->vec_or(a, b), yy));
			if (cell->type == "$xor" || cell->type == "$_XOR_")
				ez->assume(ez->vec_eq(ez->vec_xor(a, b), yy));
			if (cell->type == "$xnor")
				ez->assume(ez->vec_eq(ez->vec_not(ez->vec_xor(a, b)), yy));
			if (cell->type == "$add")
				ez->assume(ez->vec_eq(ez->vec_add(a, b), yy));
			if (cell->type == "$sub")
				ez->assume(ez->vec_eq(ez->vec_sub(a, b), yy));

			if (model_undef && !arith_undef_handled)
			{
				std::vector<int> undef_a = importUndefSigSpec(cell->connections.at("\\A"), timestep);
				std::vector<int> undef_b = importUndefSigSpec(cell->connections.at("\\B"), timestep);
				std::vector<int> undef_y = importUndefSigSpec(cell->connections.at("\\Y"), timestep);
				extendSignalWidth(undef_a, undef_b, undef_y, cell, false);

				if (cell->type == "$and" || cell->type == "$_AND_") {
					std::vector<int> a0 = ez->vec_and(ez->vec_not(a), ez->vec_not(undef_a));
					std::vector<int> b0 = ez->vec_and(ez->vec_not(b), ez->vec_not(undef_b));
					std::vector<int> yX = ez->vec_and(ez->vec_or(undef_a, undef_b), ez->vec_not(ez->vec_or(a0, b0)));
					ez->assume(ez->vec_eq(yX, undef_y));
				}
				else if (cell->type == "$or" || cell->type == "$_OR_") {
					std::vector<int> a1 = ez->vec_and(a, ez->vec_not(undef_a));
					std::vector<int> b1 = ez->vec_and(b, ez->vec_not(undef_b));
					std::vector<int> yX = ez->vec_and(ez->vec_or(undef_a, undef_b), ez->vec_not(ez->vec_or(a1, b1)));
					ez->assume(ez->vec_eq(yX, undef_y));
				}
				else if (cell->type == "$xor" || cell->type == "$_XOR_" || cell->type == "$xnor") {
					std::vector<int> yX = ez->vec_or(undef_a, undef_b);
					ez->assume(ez->vec_eq(yX, undef_y));
				}
				else
					log_abort();

				undefGating(y, yy, undef_y);
			}
			else if (model_undef)
			{
				std::vector<int> undef_y = importUndefSigSpec(cell->connections.at("\\Y"), timestep);
				undefGating(y, yy, undef_y);
			}
			return true;
		}

		if (cell->type == "$_INV_" || cell->type == "$not")
		{
			std::vector<int> a = importDefSigSpec(cell->connections.at("\\A"), timestep);
			std::vector<int> y = importDefSigSpec(cell->connections.at("\\Y"), timestep);
			extendSignalWidthUnary(a, y, cell);

			std::vector<int> yy = model_undef ? ez->vec_var(y.size()) : y;
			ez->assume(ez->vec_eq(ez->vec_not(a), yy));

			if (model_undef) {
				std::vector<int> undef_a = importUndefSigSpec(cell->connections.at("\\A"), timestep);
				std::vector<int> undef_y = importUndefSigSpec(cell->connections.at("\\Y"), timestep);
				extendSignalWidthUnary(undef_a, undef_y, cell, true);
				ez->assume(ez->vec_eq(undef_a, undef_y));
				undefGating(y, yy, undef_y);
			}
			return true;
		}

		if (cell->type == "$_MUX_" || cell->type == "$mux")
		{
			std::vector<int> a = importDefSigSpec(cell->connections.at("\\A"), timestep);
			std::vector<int> b = importDefSigSpec(cell->connections.at("\\B"), timestep);
			std::vector<int> s = importDefSigSpec(cell->connections.at("\\S"), timestep);
			std::vector<int> y = importDefSigSpec(cell->connections.at("\\Y"), timestep);

			std::vector<int> yy = model_undef ? ez->vec_var(y.size()) : y;
			ez->assume(ez->vec_eq(ez->vec_ite(s.at(0), b, a), yy));

			if (model_undef)
			{
				std::vector<int> undef_a = importUndefSigSpec(cell->connections.at("\\A"), timestep);
				std::vector<int> undef_b = importUndefSigSpec(cell->connections.at("\\B"), timestep);
				std::vector<int> undef_s = importUndefSigSpec(cell->connections.at("\\S"), timestep);
				std::vector<int> undef_y = importUndefSigSpec(cell->connections.at("\\Y"), timestep);

				std::vector<int> unequal_ab = ez->vec_not(ez->vec_iff(a, b));
				std::vector<int> undef_ab = ez->vec_or(unequal_ab, ez->vec_or(undef_a, undef_b));
				std::vector<int> yX = ez->vec_ite(undef_s.at(0), undef_ab, ez->vec_ite(s.at(0), undef_b, undef_a));
				ez->assume(ez->vec_eq(yX, undef_y));
				undefGating(y, yy, undef_y);
			}
			return true;
		}

		if (cell->type == "$pmux" || cell->type == "$safe_pmux")
		{
			std::vector<int> a = importDefSigSpec(cell->connections.at("\\A"), timestep);
			std::vector<int> b = importDefSigSpec(cell->connections.at("\\B"), timestep);
			std::vector<int> s = importDefSigSpec(cell->connections.at("\\S"), timestep);
			std::vector<int> y = importDefSigSpec(cell->connections.at("\\Y"), timestep);

			std::vector<int> yy = model_undef ? ez->vec_var(y.size()) : y;

			std::vector<int> tmp = a;
			for (size_t i = 0; i < s.size(); i++) {
				std::vector<int> part_of_b(b.begin()+i*a.size(), b.begin()+(i+1)*a.size());
				tmp = ez->vec_ite(s.at(i), part_of_b, tmp);
			}
			if (cell->type == "$safe_pmux")
				tmp = ez->vec_ite(ez->onehot(s, true), tmp, a);
			ez->assume(ez->vec_eq(tmp, yy));

			if (model_undef)
			{
				std::vector<int> undef_a = importUndefSigSpec(cell->connections.at("\\A"), timestep);
				std::vector<int> undef_b = importUndefSigSpec(cell->connections.at("\\B"), timestep);
				std::vector<int> undef_s = importUndefSigSpec(cell->connections.at("\\S"), timestep);
				std::vector<int> undef_y = importUndefSigSpec(cell->connections.at("\\Y"), timestep);

				int maybe_one_hot = ez->FALSE;
				int maybe_many_hot = ez->FALSE;

				int sure_one_hot = ez->FALSE;
				int sure_many_hot = ez->FALSE;

				std::vector<int> bits_set = std::vector<int>(undef_y.size(), ez->FALSE);
				std::vector<int> bits_clr = std::vector<int>(undef_y.size(), ez->FALSE);

				for (size_t i = 0; i < s.size(); i++)
				{
					std::vector<int> part_of_b(b.begin()+i*a.size(), b.begin()+(i+1)*a.size());
					std::vector<int> part_of_undef_b(undef_b.begin()+i*a.size(), undef_b.begin()+(i+1)*a.size());

					int maybe_s = ez->OR(s.at(i), undef_s.at(i));
					int sure_s = ez->AND(s.at(i), ez->NOT(undef_s.at(i)));

					maybe_one_hot = ez->OR(maybe_one_hot, maybe_s);
					maybe_many_hot = ez->OR(maybe_many_hot, ez->AND(maybe_one_hot, maybe_s));

					sure_one_hot = ez->OR(sure_one_hot, sure_s);
					sure_many_hot = ez->OR(sure_many_hot, ez->AND(sure_one_hot, sure_s));

					bits_set = ez->vec_ite(maybe_s, ez->vec_or(bits_set, ez->vec_or(bits_set, ez->vec_or(part_of_b, part_of_undef_b))), bits_set);
					bits_clr = ez->vec_ite(maybe_s, ez->vec_or(bits_clr, ez->vec_or(bits_clr, ez->vec_or(ez->vec_not(part_of_b), part_of_undef_b))), bits_clr);
				}

				int maybe_a = ez->NOT(maybe_one_hot);

				if (cell->type == "$safe_pmux") {
					maybe_a = ez->OR(maybe_a, maybe_many_hot);
					bits_set = ez->vec_ite(sure_many_hot, ez->vec_or(a, undef_a), bits_set);
					bits_clr = ez->vec_ite(sure_many_hot, ez->vec_or(ez->vec_not(a), undef_a), bits_clr);
				}

				bits_set = ez->vec_ite(maybe_a, ez->vec_or(bits_set, ez->vec_or(bits_set, ez->vec_or(a, undef_a))), bits_set);
				bits_clr = ez->vec_ite(maybe_a, ez->vec_or(bits_clr, ez->vec_or(bits_clr, ez->vec_or(ez->vec_not(a), undef_a))), bits_clr);

				ez->assume(ez->vec_eq(ez->vec_not(ez->vec_xor(bits_set, bits_clr)), undef_y));
				undefGating(y, yy, undef_y);
			}
			return true;
		}

		if (cell->type == "$pos" || cell->type == "$bu0" || cell->type == "$neg")
		{
			std::vector<int> a = importDefSigSpec(cell->connections.at("\\A"), timestep);
			std::vector<int> y = importDefSigSpec(cell->connections.at("\\Y"), timestep);
			extendSignalWidthUnary(a, y, cell);

			std::vector<int> yy = model_undef ? ez->vec_var(y.size()) : y;

			if (cell->type == "$pos" || cell->type == "$bu0") {
				ez->assume(ez->vec_eq(a, yy));
			} else {
				std::vector<int> zero(a.size(), ez->FALSE);
				ez->assume(ez->vec_eq(ez->vec_sub(zero, a), yy));
			}

			if (model_undef)
			{
				std::vector<int> undef_a = importUndefSigSpec(cell->connections.at("\\A"), timestep);
				std::vector<int> undef_y = importUndefSigSpec(cell->connections.at("\\Y"), timestep);
				extendSignalWidthUnary(undef_a, undef_y, cell, cell->type != "$bu0");

				if (cell->type == "$pos" || cell->type == "$bu0") {
					ez->assume(ez->vec_eq(undef_a, undef_y));
				} else {
					int undef_any_a = ez->expression(ezSAT::OpOr, undef_a);
					std::vector<int> undef_y_bits(undef_y.size(), undef_any_a);
					ez->assume(ez->vec_eq(undef_y_bits, undef_y));
				}

				undefGating(y, yy, undef_y);
			}
			return true;
		}

		if (cell->type == "$reduce_and" || cell->type == "$reduce_or" || cell->type == "$reduce_xor" ||
				cell->type == "$reduce_xnor" || cell->type == "$reduce_bool" || cell->type == "$logic_not")
		{
			std::vector<int> a = importDefSigSpec(cell->connections.at("\\A"), timestep);
			std::vector<int> y = importDefSigSpec(cell->connections.at("\\Y"), timestep);

			std::vector<int> yy = model_undef ? ez->vec_var(y.size()) : y;

			if (cell->type == "$reduce_and")
				ez->SET(ez->expression(ez->OpAnd, a), yy.at(0));
			if (cell->type == "$reduce_or" || cell->type == "$reduce_bool")
				ez->SET(ez->expression(ez->OpOr, a), yy.at(0));
			if (cell->type == "$reduce_xor")
				ez->SET(ez->expression(ez->OpXor, a), yy.at(0));
			if (cell->type == "$reduce_xnor")
				ez->SET(ez->NOT(ez->expression(ez->OpXor, a)), yy.at(0));
			if (cell->type == "$logic_not")
				ez->SET(ez->NOT(ez->expression(ez->OpOr, a)), yy.at(0));
			for (size_t i = 1; i < y.size(); i++)
				ez->SET(ez->FALSE, yy.at(i));

			if (model_undef)
			{
				std::vector<int> undef_a = importUndefSigSpec(cell->connections.at("\\A"), timestep);
				std::vector<int> undef_y = importUndefSigSpec(cell->connections.at("\\Y"), timestep);
				int aX = ez->expression(ezSAT::OpOr, undef_a);

				if (cell->type == "$reduce_and") {
					int a0 = ez->expression(ezSAT::OpOr, ez->vec_and(ez->vec_not(a), ez->vec_not(undef_a)));
					ez->assume(ez->IFF(ez->AND(ez->NOT(a0), aX), undef_y.at(0)));
				}
				else if (cell->type == "$reduce_or" || cell->type == "$reduce_bool" || cell->type == "$logic_not") {
					int a1 = ez->expression(ezSAT::OpOr, ez->vec_and(a, ez->vec_not(undef_a)));
					ez->assume(ez->IFF(ez->AND(ez->NOT(a1), aX), undef_y.at(0)));
				}
				else if (cell->type == "$reduce_xor" || cell->type == "$reduce_xnor") {
					ez->assume(ez->IFF(aX, undef_y.at(0)));
				} else
					log_abort();

				for (size_t i = 1; i < undef_y.size(); i++)
					ez->SET(ez->FALSE, undef_y.at(i));

				undefGating(y, yy, undef_y);
			}
			return true;
		}

		if (cell->type == "$logic_and" || cell->type == "$logic_or")
		{
			std::vector<int> vec_a = importDefSigSpec(cell->connections.at("\\A"), timestep);
			std::vector<int> vec_b = importDefSigSpec(cell->connections.at("\\B"), timestep);

			int a = ez->expression(ez->OpOr, vec_a);
			int b = ez->expression(ez->OpOr, vec_b);
			std::vector<int> y = importDefSigSpec(cell->connections.at("\\Y"), timestep);

			std::vector<int> yy = model_undef ? ez->vec_var(y.size()) : y;

			if (cell->type == "$logic_and")
				ez->SET(ez->expression(ez->OpAnd, a, b), yy.at(0));
			else
				ez->SET(ez->expression(ez->OpOr, a, b), yy.at(0));
			for (size_t i = 1; i < y.size(); i++)
				ez->SET(ez->FALSE, yy.at(i));

			if (model_undef)
			{
				std::vector<int> undef_a = importUndefSigSpec(cell->connections.at("\\A"), timestep);
				std::vector<int> undef_b = importUndefSigSpec(cell->connections.at("\\B"), timestep);
				std::vector<int> undef_y = importUndefSigSpec(cell->connections.at("\\Y"), timestep);

				int a0 = ez->NOT(ez->OR(ez->expression(ezSAT::OpOr, vec_a), ez->expression(ezSAT::OpOr, undef_a)));
				int b0 = ez->NOT(ez->OR(ez->expression(ezSAT::OpOr, vec_b), ez->expression(ezSAT::OpOr, undef_b)));
				int a1 = ez->expression(ezSAT::OpOr, ez->vec_and(vec_a, ez->vec_not(undef_a)));
				int b1 = ez->expression(ezSAT::OpOr, ez->vec_and(vec_b, ez->vec_not(undef_b)));
				int aX = ez->expression(ezSAT::OpOr, undef_a);
				int bX = ez->expression(ezSAT::OpOr, undef_b);

				if (cell->type == "$logic_and")
					ez->SET(ez->AND(ez->OR(aX, bX), ez->NOT(ez->AND(a1, b1)), ez->NOT(a0), ez->NOT(b0)), undef_y.at(0));
				else if (cell->type == "$logic_or")
					ez->SET(ez->AND(ez->OR(aX, bX), ez->NOT(ez->AND(a0, b0)), ez->NOT(a1), ez->NOT(b1)), undef_y.at(0));
				else
					log_abort();

				for (size_t i = 1; i < undef_y.size(); i++)
					ez->SET(ez->FALSE, undef_y.at(i));

				undefGating(y, yy, undef_y);
			}
			return true;
		}

		if (cell->type == "$lt" || cell->type == "$le" || cell->type == "$eq" || cell->type == "$ne" || cell->type == "$eqx" || cell->type == "$nex" || cell->type == "$ge" || cell->type == "$gt")
		{
			bool is_signed = cell->parameters["\\A_SIGNED"].as_bool() && cell->parameters["\\B_SIGNED"].as_bool();
			std::vector<int> a = importDefSigSpec(cell->connections.at("\\A"), timestep);
			std::vector<int> b = importDefSigSpec(cell->connections.at("\\B"), timestep);
			std::vector<int> y = importDefSigSpec(cell->connections.at("\\Y"), timestep);
			extendSignalWidth(a, b, cell);

			std::vector<int> yy = model_undef ? ez->vec_var(y.size()) : y;

			if (model_undef && (cell->type == "$eqx" || cell->type == "$nex")) {
				std::vector<int> undef_a = importUndefSigSpec(cell->connections.at("\\A"), timestep);
				std::vector<int> undef_b = importUndefSigSpec(cell->connections.at("\\B"), timestep);
				extendSignalWidth(undef_a, undef_b, cell, true);
				a = ez->vec_or(a, undef_a);
				b = ez->vec_or(b, undef_b);
			}

			if (cell->type == "$lt")
				ez->SET(is_signed ? ez->vec_lt_signed(a, b) : ez->vec_lt_unsigned(a, b), yy.at(0));
			if (cell->type == "$le")
				ez->SET(is_signed ? ez->vec_le_signed(a, b) : ez->vec_le_unsigned(a, b), yy.at(0));
			if (cell->type == "$eq" || cell->type == "$eqx")
				ez->SET(ez->vec_eq(a, b), yy.at(0));
			if (cell->type == "$ne" || cell->type == "$nex")
				ez->SET(ez->vec_ne(a, b), yy.at(0));
			if (cell->type == "$ge")
				ez->SET(is_signed ? ez->vec_ge_signed(a, b) : ez->vec_ge_unsigned(a, b), yy.at(0));
			if (cell->type == "$gt")
				ez->SET(is_signed ? ez->vec_gt_signed(a, b) : ez->vec_gt_unsigned(a, b), yy.at(0));
			for (size_t i = 1; i < y.size(); i++)
				ez->SET(ez->FALSE, yy.at(i));

			if (model_undef && (cell->type == "$eqx" || cell->type == "$nex"))
			{
				std::vector<int> undef_a = importUndefSigSpec(cell->connections.at("\\A"), timestep);
				std::vector<int> undef_b = importUndefSigSpec(cell->connections.at("\\B"), timestep);
				std::vector<int> undef_y = importUndefSigSpec(cell->connections.at("\\Y"), timestep);
				extendSignalWidth(undef_a, undef_b, cell, true);

				if (cell->type == "$eqx")
					yy.at(0) = ez->AND(yy.at(0), ez->vec_eq(undef_a, undef_b));
				else
					yy.at(0) = ez->OR(yy.at(0), ez->vec_ne(undef_a, undef_b));

				for (size_t i = 0; i < y.size(); i++)
					ez->SET(ez->FALSE, undef_y.at(i));

				ez->assume(ez->vec_eq(y, yy));
			}
			else if (model_undef && (cell->type == "$eq" || cell->type == "$ne"))
			{
				std::vector<int> undef_a = importUndefSigSpec(cell->connections.at("\\A"), timestep);
				std::vector<int> undef_b = importUndefSigSpec(cell->connections.at("\\B"), timestep);
				std::vector<int> undef_y = importUndefSigSpec(cell->connections.at("\\Y"), timestep);
				extendSignalWidth(undef_a, undef_b, cell, true);

				int undef_any_a = ez->expression(ezSAT::OpOr, undef_a);
				int undef_any_b = ez->expression(ezSAT::OpOr, undef_b);
				int undef_any = ez->OR(undef_any_a, undef_any_b);

				std::vector<int> masked_a_bits = ez->vec_or(a, ez->vec_or(undef_a, undef_b));
				std::vector<int> masked_b_bits = ez->vec_or(b, ez->vec_or(undef_a, undef_b));

				int masked_ne = ez->vec_ne(masked_a_bits, masked_b_bits);
				int undef_y_bit = ez->AND(undef_any, ez->NOT(masked_ne));

				for (size_t i = 1; i < undef_y.size(); i++)
					ez->SET(ez->FALSE, undef_y.at(i));
				ez->SET(undef_y_bit, undef_y.at(0));

				undefGating(y, yy, undef_y);
			}
			else
			{
				if (model_undef) {
					std::vector<int> undef_y = importUndefSigSpec(cell->connections.at("\\Y"), timestep);
					undefGating(y, yy, undef_y);
				}
				log_assert(!model_undef || arith_undef_handled);
			}
			return true;
		}

		if (cell->type == "$shl" || cell->type == "$shr" || cell->type == "$sshl" || cell->type == "$sshr")
		{
			std::vector<int> a = importDefSigSpec(cell->connections.at("\\A"), timestep);
			std::vector<int> b = importDefSigSpec(cell->connections.at("\\B"), timestep);
			std::vector<int> y = importDefSigSpec(cell->connections.at("\\Y"), timestep);

			char shift_left = cell->type == "$shl" || cell->type == "$sshl";
			bool sign_extend = cell->type == "$sshr" && cell->parameters["\\A_SIGNED"].as_bool();

			while (y.size() < a.size())
				y.push_back(ez->literal());
			while (y.size() > a.size())
				a.push_back(cell->parameters["\\A_SIGNED"].as_bool() ? a.back() : ez->FALSE);

			std::vector<int> yy = model_undef ? ez->vec_var(y.size()) : y;

			std::vector<int> tmp = a;
			for (size_t i = 0; i < b.size(); i++)
			{
				std::vector<int> tmp_shifted(tmp.size());
				for (size_t j = 0; j < tmp.size(); j++) {
					int idx = j + (1 << (i > 30 ? 30 : i)) * (shift_left ? -1 : +1);
					tmp_shifted.at(j) = (0 <= idx && idx < int(tmp.size())) ? tmp.at(idx) : sign_extend ? tmp.back() : ez->FALSE;
				}
				tmp = ez->vec_ite(b.at(i), tmp_shifted, tmp);
			}
			ez->assume(ez->vec_eq(tmp, yy));

			if (model_undef)
			{
				std::vector<int> undef_a = importUndefSigSpec(cell->connections.at("\\A"), timestep);
				std::vector<int> undef_b = importUndefSigSpec(cell->connections.at("\\B"), timestep);
				std::vector<int> undef_y = importUndefSigSpec(cell->connections.at("\\Y"), timestep);

				while (undef_y.size() < undef_a.size())
					undef_y.push_back(ez->literal());
				while (undef_y.size() > undef_a.size())
					undef_a.push_back(cell->parameters["\\A_SIGNED"].as_bool() ? undef_a.back() : ez->FALSE);

				tmp = undef_a;
				for (size_t i = 0; i < b.size(); i++)
				{
					std::vector<int> tmp_shifted(tmp.size());
					for (size_t j = 0; j < tmp.size(); j++) {
						int idx = j + (1 << (i > 30 ? 30 : i)) * (shift_left ? -1 : +1);
						tmp_shifted.at(j) = (0 <= idx && idx < int(tmp.size())) ? tmp.at(idx) : sign_extend ? tmp.back() : ez->FALSE;
					}
					tmp = ez->vec_ite(b.at(i), tmp_shifted, tmp);
				}

				int undef_any_b = ez->expression(ezSAT::OpOr, undef_b);
				std::vector<int> undef_all_y_bits(undef_y.size(), undef_any_b);
				ez->assume(ez->vec_eq(ez->vec_or(tmp, undef_all_y_bits), undef_y));
				undefGating(y, yy, undef_y);
			}
			return true;
		}

		if (cell->type == "$mul")
		{
			std::vector<int> a = importDefSigSpec(cell->connections.at("\\A"), timestep);
			std::vector<int> b = importDefSigSpec(cell->connections.at("\\B"), timestep);
			std::vector<int> y = importDefSigSpec(cell->connections.at("\\Y"), timestep);
			extendSignalWidth(a, b, y, cell);

			std::vector<int> yy = model_undef ? ez->vec_var(y.size()) : y;

			std::vector<int> tmp(a.size(), ez->FALSE);
			for (int i = 0; i < int(a.size()); i++)
			{
				std::vector<int> shifted_a(a.size(), ez->FALSE);
				for (int j = i; j < int(a.size()); j++)
					shifted_a.at(j) = a.at(j-i);
				tmp = ez->vec_ite(b.at(i), ez->vec_add(tmp, shifted_a), tmp);
			}
			ez->assume(ez->vec_eq(tmp, yy));

			if (model_undef) {
				log_assert(arith_undef_handled);
				std::vector<int> undef_y = importUndefSigSpec(cell->connections.at("\\Y"), timestep);
				undefGating(y, yy, undef_y);
			}
			return true;
		}

		if (cell->type == "$div" || cell->type == "$mod")
		{
			std::vector<int> a = importDefSigSpec(cell->connections.at("\\A"), timestep);
			std::vector<int> b = importDefSigSpec(cell->connections.at("\\B"), timestep);
			std::vector<int> y = importDefSigSpec(cell->connections.at("\\Y"), timestep);
			extendSignalWidth(a, b, y, cell);

			std::vector<int> yy = model_undef ? ez->vec_var(y.size()) : y;

			std::vector<int> a_u, b_u;
			if (cell->parameters["\\A_SIGNED"].as_bool() && cell->parameters["\\B_SIGNED"].as_bool()) {
				a_u = ez->vec_ite(a.back(), ez->vec_neg(a), a);
				b_u = ez->vec_ite(b.back(), ez->vec_neg(b), b);
			} else {
				a_u = a;
				b_u = b;
			}

			std::vector<int> chain_buf = a_u;
			std::vector<int> y_u(a_u.size(), ez->FALSE);
			for (int i = int(a.size())-1; i >= 0; i--)
			{
				chain_buf.insert(chain_buf.end(), chain_buf.size(), ez->FALSE);

				std::vector<int> b_shl(i, ez->FALSE);
				b_shl.insert(b_shl.end(), b_u.begin(), b_u.end());
				b_shl.insert(b_shl.end(), chain_buf.size()-b_shl.size(), ez->FALSE);

				y_u.at(i) = ez->vec_ge_unsigned(chain_buf, b_shl);
				chain_buf = ez->vec_ite(y_u.at(i), ez->vec_sub(chain_buf, b_shl), chain_buf);

				chain_buf.erase(chain_buf.begin() + a_u.size(), chain_buf.end());
			}

			std::vector<int> y_tmp = ignore_div_by_zero ? yy : ez->vec_var(y.size());
			if (cell->type == "$div") {
				if (cell->parameters["\\A_SIGNED"].as_bool() && cell->parameters["\\B_SIGNED"].as_bool())
					ez->assume(ez->vec_eq(y_tmp, ez->vec_ite(ez->XOR(a.back(), b.back()), ez->vec_neg(y_u), y_u)));
				else
					ez->assume(ez->vec_eq(y_tmp, y_u));
			} else {
				if (cell->parameters["\\A_SIGNED"].as_bool() && cell->parameters["\\B_SIGNED"].as_bool())
					ez->assume(ez->vec_eq(y_tmp, ez->vec_ite(a.back(), ez->vec_neg(chain_buf), chain_buf)));
				else
					ez->assume(ez->vec_eq(y_tmp, chain_buf));
			}

			if (ignore_div_by_zero) {
				ez->assume(ez->expression(ezSAT::OpOr, b));
			} else {
				std::vector<int> div_zero_result;
				if (cell->type == "$div") {
					if (cell->parameters["\\A_SIGNED"].as_bool() && cell->parameters["\\B_SIGNED"].as_bool()) {
						std::vector<int> all_ones(y.size(), ez->TRUE);
						std::vector<int> only_first_one(y.size(), ez->FALSE);
						only_first_one.at(0) = ez->TRUE;
						div_zero_result = ez->vec_ite(a.back(), only_first_one, all_ones);
					} else {
						div_zero_result.insert(div_zero_result.end(), cell->connections.at("\\A").width, ez->TRUE);
						div_zero_result.insert(div_zero_result.end(), y.size() - div_zero_result.size(), ez->FALSE);
					}
				} else {
					int copy_a_bits = std::min(cell->connections.at("\\A").width, cell->connections.at("\\B").width);
					div_zero_result.insert(div_zero_result.end(), a.begin(), a.begin() + copy_a_bits);
					if (cell->parameters["\\A_SIGNED"].as_bool() && cell->parameters["\\B_SIGNED"].as_bool())
						div_zero_result.insert(div_zero_result.end(), y.size() - div_zero_result.size(), div_zero_result.back());
					else
						div_zero_result.insert(div_zero_result.end(), y.size() - div_zero_result.size(), ez->FALSE);
				}
				ez->assume(ez->vec_eq(yy, ez->vec_ite(ez->expression(ezSAT::OpOr, b), y_tmp, div_zero_result)));
			}

			if (model_undef) {
				log_assert(arith_undef_handled);
				std::vector<int> undef_y = importUndefSigSpec(cell->connections.at("\\Y"), timestep);
				undefGating(y, yy, undef_y);
			}
			return true;
		}

		if (cell->type == "$slice")
		{
			RTLIL::SigSpec a = cell->connections.at("\\A");
			RTLIL::SigSpec y = cell->connections.at("\\Y");
			ez->assume(signals_eq(a.extract(cell->parameters.at("\\OFFSET").as_int(), y.width), y, timestep));
			return true;
		}

		if (cell->type == "$concat")
		{
			RTLIL::SigSpec a = cell->connections.at("\\A");
			RTLIL::SigSpec b = cell->connections.at("\\B");
			RTLIL::SigSpec y = cell->connections.at("\\Y");

			RTLIL::SigSpec ab = a;
			ab.append(b);

			ez->assume(signals_eq(ab, y, timestep));
			return true;
		}

		if (timestep > 0 && (cell->type == "$dff" || cell->type == "$_DFF_N_" || cell->type == "$_DFF_P_"))
		{
			if (timestep == 1)
			{
				initial_state.add((*sigmap)(cell->connections.at("\\Q")));
			}
			else
			{
				std::vector<int> d = importDefSigSpec(cell->connections.at("\\D"), timestep-1);
				std::vector<int> q = importDefSigSpec(cell->connections.at("\\Q"), timestep);

				std::vector<int> qq = model_undef ? ez->vec_var(q.size()) : q;
				ez->assume(ez->vec_eq(d, qq));

				if (model_undef)
				{
					std::vector<int> undef_d = importUndefSigSpec(cell->connections.at("\\D"), timestep-1);
					std::vector<int> undef_q = importUndefSigSpec(cell->connections.at("\\Q"), timestep);

					ez->assume(ez->vec_eq(undef_d, undef_q));
					undefGating(q, qq, undef_q);
				}
			}
			return true;
		}

		if (cell->type == "$assert")
		{
			std::string pf = prefix + (timestep == -1 ? "" : stringf("@%d:", timestep));
			asserts_a[pf].append((*sigmap)(cell->connections.at("\\A")));
			asserts_en[pf].append((*sigmap)(cell->connections.at("\\EN")));
			return true;
		}

		// Unsupported internal cell types: $pow $lut
		// .. and all sequential cells except $dff and $_DFF_[NP]_
		return false;
	}
};

#endif

