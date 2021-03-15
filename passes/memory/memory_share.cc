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

#include "kernel/yosys.h"
#include "kernel/satgen.h"
#include "kernel/sigtools.h"
#include "kernel/modtools.h"
#include "kernel/mem.h"

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

struct MemoryShareWorker
{
	RTLIL::Design *design;
	RTLIL::Module *module;
	SigMap sigmap, sigmap_xmux;
	ModWalker modwalker;
	CellTypes cone_ct;

	std::map<RTLIL::SigBit, std::pair<RTLIL::Cell*, int>> sig_to_mux;
	std::map<pair<std::set<std::map<SigBit, bool>>, SigBit>, SigBit> conditions_logic_cache;
	std::vector<Mem> memories;


	// -----------------------------------------------------------------
	// Converting feedbacks to async read ports to proper enable signals
	// -----------------------------------------------------------------

	bool find_data_feedback(const std::set<RTLIL::SigBit> &async_rd_bits, RTLIL::SigBit sig,
			std::map<RTLIL::SigBit, bool> &state, std::set<std::map<RTLIL::SigBit, bool>> &conditions)
	{
		if (async_rd_bits.count(sig)) {
			conditions.insert(state);
			return true;
		}

		if (sig_to_mux.count(sig) == 0)
			return false;

		RTLIL::Cell *cell = sig_to_mux.at(sig).first;
		int bit_idx = sig_to_mux.at(sig).second;

		std::vector<RTLIL::SigBit> sig_a = sigmap(cell->getPort(ID::A));
		std::vector<RTLIL::SigBit> sig_b = sigmap(cell->getPort(ID::B));
		std::vector<RTLIL::SigBit> sig_s = sigmap(cell->getPort(ID::S));
		std::vector<RTLIL::SigBit> sig_y = sigmap(cell->getPort(ID::Y));
		log_assert(sig_y.at(bit_idx) == sig);

		for (int i = 0; i < int(sig_s.size()); i++)
			if (state.count(sig_s[i]) && state.at(sig_s[i]) == true) {
				if (find_data_feedback(async_rd_bits, sig_b.at(bit_idx + i*sig_y.size()), state, conditions)) {
					RTLIL::SigSpec new_b = cell->getPort(ID::B);
					new_b.replace(bit_idx + i*sig_y.size(), RTLIL::State::Sx);
					cell->setPort(ID::B, new_b);
				}
				return false;
			}


		for (int i = 0; i < int(sig_s.size()); i++)
		{
			if (state.count(sig_s[i]) && state.at(sig_s[i]) == false)
				continue;

			std::map<RTLIL::SigBit, bool> new_state = state;
			new_state[sig_s[i]] = true;

			if (find_data_feedback(async_rd_bits, sig_b.at(bit_idx + i*sig_y.size()), new_state, conditions)) {
				RTLIL::SigSpec new_b = cell->getPort(ID::B);
				new_b.replace(bit_idx + i*sig_y.size(), RTLIL::State::Sx);
				cell->setPort(ID::B, new_b);
			}
		}

		std::map<RTLIL::SigBit, bool> new_state = state;
		for (int i = 0; i < int(sig_s.size()); i++)
			new_state[sig_s[i]] = false;

		if (find_data_feedback(async_rd_bits, sig_a.at(bit_idx), new_state, conditions)) {
			RTLIL::SigSpec new_a = cell->getPort(ID::A);
			new_a.replace(bit_idx, RTLIL::State::Sx);
			cell->setPort(ID::A, new_a);
		}

		return false;
	}

	RTLIL::SigBit conditions_to_logic(std::set<std::map<RTLIL::SigBit, bool>> &conditions, SigBit olden, int &created_conditions)
	{
		auto key = make_pair(conditions, olden);

		if (conditions_logic_cache.count(key))
			return conditions_logic_cache.at(key);

		RTLIL::SigSpec terms;
		for (auto &cond : conditions) {
			RTLIL::SigSpec sig1, sig2;
			for (auto &it : cond) {
				sig1.append(it.first);
				sig2.append(it.second ? RTLIL::State::S1 : RTLIL::State::S0);
			}
			terms.append(module->Ne(NEW_ID, sig1, sig2));
			created_conditions++;
		}

		if (olden.wire != nullptr || olden != State::S1)
			terms.append(olden);

		if (GetSize(terms) == 0)
			terms = State::S1;

		if (GetSize(terms) > 1)
			terms = module->ReduceAnd(NEW_ID, terms);

		return conditions_logic_cache[key] = terms;
	}

	void translate_rd_feedback_to_en(Mem &mem)
	{
		std::map<RTLIL::SigSpec, std::vector<std::set<RTLIL::SigBit>>> async_rd_bits;
		std::map<RTLIL::SigBit, std::set<RTLIL::SigBit>> muxtree_upstream_map;
		std::set<RTLIL::SigBit> non_feedback_nets;

		for (auto wire : module->wires())
			if (wire->port_output) {
				std::vector<RTLIL::SigBit> bits = sigmap(wire);
				non_feedback_nets.insert(bits.begin(), bits.end());
			}

		for (auto cell : module->cells())
		{
			bool ignore_data_port = false;

			if (cell->type.in(ID($mux), ID($pmux)))
			{
				std::vector<RTLIL::SigBit> sig_a = sigmap(cell->getPort(ID::A));
				std::vector<RTLIL::SigBit> sig_b = sigmap(cell->getPort(ID::B));
				std::vector<RTLIL::SigBit> sig_s = sigmap(cell->getPort(ID::S));
				std::vector<RTLIL::SigBit> sig_y = sigmap(cell->getPort(ID::Y));

				non_feedback_nets.insert(sig_s.begin(), sig_s.end());

				for (int i = 0; i < int(sig_y.size()); i++) {
					muxtree_upstream_map[sig_y[i]].insert(sig_a[i]);
					for (int j = 0; j < int(sig_s.size()); j++)
						muxtree_upstream_map[sig_y[i]].insert(sig_b[i + j*sig_y.size()]);
				}

				continue;
			}

			if (cell->type.in(ID($memwr), ID($memrd)) &&
					IdString(cell->parameters.at(ID::MEMID).decode_string()) == mem.memid)
				ignore_data_port = true;

			for (auto conn : cell->connections())
			{
				if (ignore_data_port && conn.first == ID::DATA)
					continue;
				std::vector<RTLIL::SigBit> bits = sigmap(conn.second);
				non_feedback_nets.insert(bits.begin(), bits.end());
			}
		}

		std::set<RTLIL::SigBit> expand_non_feedback_nets = non_feedback_nets;
		while (!expand_non_feedback_nets.empty())
		{
			std::set<RTLIL::SigBit> new_expand_non_feedback_nets;

			for (auto &bit : expand_non_feedback_nets)
				if (muxtree_upstream_map.count(bit))
					for (auto &new_bit : muxtree_upstream_map.at(bit))
						if (!non_feedback_nets.count(new_bit)) {
							non_feedback_nets.insert(new_bit);
							new_expand_non_feedback_nets.insert(new_bit);
						}

			expand_non_feedback_nets.swap(new_expand_non_feedback_nets);
		}

		for (auto &port : mem.rd_ports)
		{
			if (port.clk_enable)
				continue;

			for (auto &bit : port.data)
				if (non_feedback_nets.count(bit))
					goto not_pure_feedback_port;

			for (int sub = 0; sub < (1 << port.wide_log2); sub++) {
				SigSpec addr = port.addr;
				for (int i = 0; i < port.wide_log2; i++)
					addr[i] = State(sub >> i & 1);
				async_rd_bits[addr].resize(mem.width);
				for (int i = 0; i < mem.width; i++)
					async_rd_bits[port.addr][i].insert(port.data[i + sub * mem.width]);
			}

		not_pure_feedback_port:;
		}

		if (async_rd_bits.empty())
			return;

		bool changed = false;
		log("Populating enable bits on write ports of memory %s.%s with aync read feedback:\n", log_id(module), log_id(mem.memid));

		for (int i = 0; i < GetSize(mem.wr_ports); i++)
		{
			auto &port = mem.wr_ports[i];

			if (!port.wide_log2 && !async_rd_bits.count(port.addr))
				continue;

			log("  Analyzing write port %d.\n", i);

			int created_conditions = 0;
			for (int j = 0; j < GetSize(port.data); j++)
				if (port.en[j] != RTLIL::SigBit(RTLIL::State::S0))
				{
					std::map<RTLIL::SigBit, bool> state;
					std::set<std::map<RTLIL::SigBit, bool>> conditions;
					int sub = j / mem.width;
					SigSpec addr = port.addr;
					for (int k = 0; k < port.wide_log2; k++)
						addr[k] = State(sub >> k & 1);

					find_data_feedback(async_rd_bits.at(addr).at(j % mem.width), port.data[j], state, conditions);
					port.en[j] = conditions_to_logic(conditions, port.en[j], created_conditions);
				}

			if (created_conditions) {
				log("    Added enable logic for %d different cases.\n", created_conditions);
				changed = true;
			}
		}

		if (changed)
			mem.emit();
	}


	// ------------------------------------------------------
	// Consolidate write ports that write to the same address
	// ------------------------------------------------------

	bool consolidate_wr_by_addr(Mem &mem)
	{
		if (GetSize(mem.wr_ports) <= 1)
			return false;

		log("Consolidating write ports of memory %s.%s by address:\n", log_id(module), log_id(mem.memid));

		bool did_anything = false;
		for (int i = 0; i < GetSize(mem.wr_ports); i++)
		{
			auto &port1 = mem.wr_ports[i];
			if (port1.removed)
				continue;
			if (!port1.clk_enable)
				continue;
			for (int j = i + 1; j < GetSize(mem.wr_ports); j++)
			{
				auto &port2 = mem.wr_ports[j];
				if (port2.removed)
					continue;
				if (!port2.clk_enable)
					continue;
				if (port1.clk != port2.clk)
					continue;
				if (port1.clk_polarity != port2.clk_polarity)
					continue;
				// The ports can still be merged if one od them can be widened.
				int wide_log2 = std::max(port1.wide_log2, port2.wide_log2);
				if (GetSize(port1.addr) <= wide_log2)
					continue;
				if (GetSize(port2.addr) <= wide_log2)
					continue;
				if (!port1.addr.extract(0, wide_log2).is_fully_const())
					continue;
				if (!port2.addr.extract(0, wide_log2).is_fully_const())
					continue;
				if (sigmap_xmux(port1.addr.extract_end(wide_log2)) != sigmap_xmux(port2.addr.extract_end(wide_log2))) {
					// Incompatible addresses after widening.  Last chance — widen both
					// ports by one more bit to merge them.
					wide_log2++;
					if (sigmap_xmux(port1.addr.extract_end(wide_log2)) != sigmap_xmux(port2.addr.extract_end(wide_log2)))
						continue;
					if (!port1.addr.extract(0, wide_log2).is_fully_const())
						continue;
					if (!port2.addr.extract(0, wide_log2).is_fully_const())
						continue;
				}
				log("  Merging ports %d, %d (address %s).\n", i, j, log_signal(port1.addr));
				mem.prepare_wr_merge(i, j);
				port1.addr = sigmap_xmux(port1.addr);
				port2.addr = sigmap_xmux(port2.addr);
				mem.widen_wr_port(i, wide_log2);
				mem.widen_wr_port(j, wide_log2);
				int pos = 0;
				while (pos < GetSize(port1.data)) {
					int epos = pos;
					while (epos < GetSize(port1.data) && port1.en[epos] == port1.en[pos] && port2.en[epos] == port2.en[pos])
						epos++;
					int width = epos - pos;
					SigBit new_en;
					if (port2.en[pos] == State::S0) {
						new_en = port1.en[pos];
					} else if (port1.en[pos] == State::S0) {
						port1.data.replace(pos, port2.data.extract(pos, width));
						new_en = port2.en[pos];
					} else {
						port1.data.replace(pos, module->Mux(NEW_ID, port1.data.extract(pos, width), port2.data.extract(pos, width), port2.en[pos]));
						new_en = module->Or(NEW_ID, port1.en[pos], port2.en[pos]);
					}
					for (int k = pos; k < epos; k++)
						port1.en[k] = new_en;
					pos = epos;
				}
				port2.removed = true;
				did_anything = true;
			}
		}

		if (did_anything)
			mem.emit();

		return did_anything;
	}


	// --------------------------------------------------------
	// Consolidate write ports using sat-based resource sharing
	// --------------------------------------------------------

	void consolidate_wr_using_sat(Mem &mem)
	{
		if (GetSize(mem.wr_ports) <= 1)
			return;

		ezSatPtr ez;
		SatGen satgen(ez.get(), &modwalker.sigmap);

		// find list of considered ports and port pairs

		std::set<int> considered_ports;
		std::set<int> considered_port_pairs;

		for (int i = 0; i < GetSize(mem.wr_ports); i++) {
			auto &port = mem.wr_ports[i];
			std::vector<RTLIL::SigBit> bits = modwalker.sigmap(port.en);
			for (auto bit : bits)
				if (bit == RTLIL::State::S1)
					goto port_is_always_active;
			if (modwalker.has_drivers(bits))
				considered_ports.insert(i);
		port_is_always_active:;
		}

		log("Consolidating write ports of memory %s.%s using sat-based resource sharing:\n", log_id(module), log_id(mem.memid));

		bool cache_clk_enable = false;
		bool cache_clk_polarity = false;
		RTLIL::SigSpec cache_clk;
		int cache_wide_log2 = 0;

		for (int i = 0; i < GetSize(mem.wr_ports); i++)
		{
			auto &port = mem.wr_ports[i];

			if (port.clk_enable != cache_clk_enable ||
					port.wide_log2 != cache_wide_log2 ||
					(cache_clk_enable && (sigmap(port.clk) != cache_clk ||
					port.clk_polarity != cache_clk_polarity)))
			{
				cache_clk_enable = port.clk_enable;
				cache_clk_polarity = port.clk_polarity;
				cache_clk = sigmap(port.clk);
				cache_wide_log2 = port.wide_log2;
			}
			else if (i > 0 && considered_ports.count(i-1) && considered_ports.count(i))
				considered_port_pairs.insert(i);

			if (cache_clk_enable)
				log("  Port %d on %s %s: %s\n", i,
						cache_clk_polarity ? "posedge" : "negedge", log_signal(cache_clk),
						considered_ports.count(i) ? "considered" : "not considered");
			else
				log("  Port %d unclocked: %s\n", i,
						considered_ports.count(i) ? "considered" : "not considered");
		}

		if (considered_port_pairs.size() < 1) {
			log("  No two subsequent ports in same clock domain considered -> nothing to consolidate.\n");
			return;
		}

		// create SAT representation of common input cone of all considered EN signals

		pool<Wire*> one_hot_wires;
		std::set<RTLIL::Cell*> sat_cells;
		std::set<RTLIL::SigBit> bits_queue;
		std::map<int, int> port_to_sat_variable;

		for (int i = 0; i < GetSize(mem.wr_ports); i++)
			if (considered_port_pairs.count(i) || considered_port_pairs.count(i+1))
			{
				RTLIL::SigSpec sig = modwalker.sigmap(mem.wr_ports[i].en);
				port_to_sat_variable[i] = ez->expression(ez->OpOr, satgen.importSigSpec(sig));

				std::vector<RTLIL::SigBit> bits = sig;
				bits_queue.insert(bits.begin(), bits.end());
			}

		while (!bits_queue.empty())
		{
			for (auto bit : bits_queue)
				if (bit.wire && bit.wire->get_bool_attribute(ID::onehot))
					one_hot_wires.insert(bit.wire);

			pool<ModWalker::PortBit> portbits;
			modwalker.get_drivers(portbits, bits_queue);
			bits_queue.clear();

			for (auto &pbit : portbits)
				if (sat_cells.count(pbit.cell) == 0 && cone_ct.cell_known(pbit.cell->type)) {
					pool<RTLIL::SigBit> &cell_inputs = modwalker.cell_inputs[pbit.cell];
					bits_queue.insert(cell_inputs.begin(), cell_inputs.end());
					sat_cells.insert(pbit.cell);
				}
		}

		for (auto wire : one_hot_wires) {
			log("  Adding one-hot constraint for wire %s.\n", log_id(wire));
			vector<int> ez_wire_bits = satgen.importSigSpec(wire);
			for (int i : ez_wire_bits)
			for (int j : ez_wire_bits)
				if (i != j) ez->assume(ez->NOT(i), j);
		}

		log("  Common input cone for all EN signals: %d cells.\n", int(sat_cells.size()));

		for (auto cell : sat_cells)
			satgen.importCell(cell);

		log("  Size of unconstrained SAT problem: %d variables, %d clauses\n", ez->numCnfVariables(), ez->numCnfClauses());

		// merge subsequent ports if possible

		bool did_anything = false;
		for (int i = 0; i < GetSize(mem.wr_ports); i++)
		{
			if (!considered_port_pairs.count(i))
				continue;

			if (ez->solve(port_to_sat_variable.at(i-1), port_to_sat_variable.at(i))) {
				log("  According to SAT solver sharing of port %d with port %d is not possible.\n", i-1, i);
				continue;
			}

			log("  Merging port %d into port %d.\n", i-1, i);
			port_to_sat_variable.at(i) = ez->OR(port_to_sat_variable.at(i-1), port_to_sat_variable.at(i));

			RTLIL::SigSpec last_addr = mem.wr_ports[i-1].addr;
			RTLIL::SigSpec last_data = mem.wr_ports[i-1].data;
			std::vector<RTLIL::SigBit> last_en = modwalker.sigmap(mem.wr_ports[i-1].en);

			RTLIL::SigSpec this_addr = mem.wr_ports[i].addr;
			RTLIL::SigSpec this_data = mem.wr_ports[i].data;
			std::vector<RTLIL::SigBit> this_en = modwalker.sigmap(mem.wr_ports[i].en);

			RTLIL::SigBit this_en_active = module->ReduceOr(NEW_ID, this_en);

			if (GetSize(last_addr) < GetSize(this_addr))
				last_addr.extend_u0(GetSize(this_addr));
			else
				this_addr.extend_u0(GetSize(last_addr));

			mem.wr_ports[i].addr = module->Mux(NEW_ID, last_addr, this_addr, this_en_active);
			mem.wr_ports[i].data = module->Mux(NEW_ID, last_data, this_data, this_en_active);

			std::map<std::pair<RTLIL::SigBit, RTLIL::SigBit>, int> groups_en;
			RTLIL::SigSpec grouped_last_en, grouped_this_en, en;
			RTLIL::Wire *grouped_en = module->addWire(NEW_ID, 0);

			for (int j = 0; j < int(this_en.size()); j++) {
				std::pair<RTLIL::SigBit, RTLIL::SigBit> key(last_en[j], this_en[j]);
				if (!groups_en.count(key)) {
					grouped_last_en.append(last_en[j]);
					grouped_this_en.append(this_en[j]);
					groups_en[key] = grouped_en->width;
					grouped_en->width++;
				}
				en.append(RTLIL::SigSpec(grouped_en, groups_en[key]));
			}

			module->addMux(NEW_ID, grouped_last_en, grouped_this_en, this_en_active, grouped_en);
			mem.wr_ports[i].en = en;

			mem.wr_ports[i-1].removed = true;
			did_anything = true;
		}

		if (did_anything)
			mem.emit();
	}


	// -------------
	// Setup and run
	// -------------

	MemoryShareWorker(RTLIL::Design *design) : design(design), modwalker(design) {}

	void operator()(RTLIL::Module* module)
	{
		memories = Mem::get_selected_memories(module);

		this->module = module;
		sigmap.set(module);
		sig_to_mux.clear();
		conditions_logic_cache.clear();

		sigmap_xmux = sigmap;
		for (auto cell : module->cells())
		{
			if (cell->type == ID($mux))
			{
				RTLIL::SigSpec sig_a = sigmap_xmux(cell->getPort(ID::A));
				RTLIL::SigSpec sig_b = sigmap_xmux(cell->getPort(ID::B));

				if (sig_a.is_fully_undef())
					sigmap_xmux.add(cell->getPort(ID::Y), sig_b);
				else if (sig_b.is_fully_undef())
					sigmap_xmux.add(cell->getPort(ID::Y), sig_a);
			}

			if (cell->type.in(ID($mux), ID($pmux)))
			{
				std::vector<RTLIL::SigBit> sig_y = sigmap(cell->getPort(ID::Y));
				for (int i = 0; i < int(sig_y.size()); i++)
					sig_to_mux[sig_y[i]] = std::pair<RTLIL::Cell*, int>(cell, i);
			}
		}

		for (auto &mem : memories) {
			translate_rd_feedback_to_en(mem);
			while (consolidate_wr_by_addr(mem));
		}

		cone_ct.setup_internals();
		cone_ct.cell_types.erase(ID($mul));
		cone_ct.cell_types.erase(ID($mod));
		cone_ct.cell_types.erase(ID($div));
		cone_ct.cell_types.erase(ID($modfloor));
		cone_ct.cell_types.erase(ID($divfloor));
		cone_ct.cell_types.erase(ID($pow));
		cone_ct.cell_types.erase(ID($shl));
		cone_ct.cell_types.erase(ID($shr));
		cone_ct.cell_types.erase(ID($sshl));
		cone_ct.cell_types.erase(ID($sshr));
		cone_ct.cell_types.erase(ID($shift));
		cone_ct.cell_types.erase(ID($shiftx));

		modwalker.setup(module, &cone_ct);

		for (auto &mem : memories)
			consolidate_wr_using_sat(mem);
	}
};

struct MemorySharePass : public Pass {
	MemorySharePass() : Pass("memory_share", "consolidate memory ports") { }
	void help() override
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    memory_share [selection]\n");
		log("\n");
		log("This pass merges share-able memory ports into single memory ports.\n");
		log("\n");
		log("The following methods are used to consolidate the number of memory ports:\n");
		log("\n");
		log("  - When write ports are connected to async read ports accessing the same\n");
		log("    address, then this feedback path is converted to a write port with\n");
		log("    byte/part enable signals.\n");
		log("\n");
		log("  - When multiple write ports access the same address then this is converted\n");
		log("    to a single write port with a more complex data and/or enable logic path.\n");
		log("\n");
		log("  - When multiple write ports are never accessed at the same time (a SAT\n");
		log("    solver is used to determine this), then the ports are merged into a single\n");
		log("    write port.\n");
		log("\n");
		log("Note that in addition to the algorithms implemented in this pass, the $memrd\n");
		log("and $memwr cells are also subject to generic resource sharing passes (and other\n");
		log("optimizations) such as \"share\" and \"opt_merge\".\n");
		log("\n");
	}
	void execute(std::vector<std::string> args, RTLIL::Design *design) override {
		log_header(design, "Executing MEMORY_SHARE pass (consolidating $memrd/$memwr cells).\n");
		extra_args(args, 1, design);
		MemoryShareWorker msw(design);

		for (auto module : design->selected_modules())
			msw(module);
	}
} MemorySharePass;

PRIVATE_NAMESPACE_END
