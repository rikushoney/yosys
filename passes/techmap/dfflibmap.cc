/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Copyright (C) 2012  Claire Xenia Wolf <claire@yosyshq.com>
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
#include "kernel/sigtools.h"
#include "kernel/gzip.h"
#include "libparse.h"
#include <string.h>
#include <errno.h>

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

struct cell_mapping {
	IdString cell_name;
	std::map<std::string, char> ports;
};
static std::map<RTLIL::IdString, cell_mapping> cell_mappings;

static void logmap(IdString dff)
{
	if (cell_mappings.count(dff) == 0) {
		log("    unmapped dff cell: %s\n", dff.c_str());
	} else {
		log("    %s %s (", cell_mappings[dff].cell_name.c_str(), dff.substr(1).c_str());
		bool first = true;
		for (auto &port : cell_mappings[dff].ports) {
			char arg[3] = { port.second, 0, 0 };
			if ('a' <= arg[0] && arg[0] <= 'z')
				arg[1] = arg[0] - ('a' - 'A'), arg[0] = '~';
			else
				arg[1] = arg[0], arg[0] = ' ';
			log("%s.%s(%s)", first ? "" : ", ", port.first.c_str(), arg);
			first = false;
		}
		log(");\n");
	}
}

static void logmap_all()
{
	logmap(ID($_DFF_N_));
	logmap(ID($_DFF_P_));

	logmap(ID($_DFF_NN0_));
	logmap(ID($_DFF_NN1_));
	logmap(ID($_DFF_NP0_));
	logmap(ID($_DFF_NP1_));
	logmap(ID($_DFF_PN0_));
	logmap(ID($_DFF_PN1_));
	logmap(ID($_DFF_PP0_));
	logmap(ID($_DFF_PP1_));

	logmap(ID($_DFFE_NN_));
	logmap(ID($_DFFE_NP_));
	logmap(ID($_DFFE_PN_));
	logmap(ID($_DFFE_PP_));

	logmap(ID($_DFFSR_NNN_));
	logmap(ID($_DFFSR_NNP_));
	logmap(ID($_DFFSR_NPN_));
	logmap(ID($_DFFSR_NPP_));
	logmap(ID($_DFFSR_PNN_));
	logmap(ID($_DFFSR_PNP_));
	logmap(ID($_DFFSR_PPN_));
	logmap(ID($_DFFSR_PPP_));
}

static bool parse_next_state(const LibertyAst *cell, const LibertyAst *attr, std::string &data_name, bool &data_not_inverted, std::string &enable_name, bool &enable_not_inverted)
{
	static pool<std::string> warned_cells{};

	if (cell == nullptr || attr == nullptr || attr->value.empty())
		return false;

	auto expr = attr->value;
	auto cell_name = cell->args[0];

	for (size_t pos = expr.find_first_of("\"\t"); pos != std::string::npos; pos = expr.find_first_of("\"\t"))
		expr.erase(pos, 1);

	// if this isn't an enable flop, the next_state variable is usually just the input pin name.
	if (expr[expr.size()-1] == '\'') {
		data_name = expr.substr(0, expr.size()-1);
		data_not_inverted = false;
	} else if (expr[0] == '!') {
		data_name = expr.substr(1, expr.size()-1);
		data_not_inverted = false;
	} else if (expr[0] == '(' && expr[expr.size() - 1] == ')') {
		data_name = expr.substr(1, expr.size() - 2);
		data_not_inverted = true;
	} else {
		data_name = expr;
		data_not_inverted = true;
	}

	for (auto child : cell->children)
		if (child->id == "pin" && child->args.size() == 1 && child->args[0] == data_name)
			return true;

	// the next_state variable isn't just a pin name; perhaps this is an enable?
	auto helper = LibertyExpression::Lexer(expr);
	auto tree = LibertyExpression::parse(helper);
	// log_debug("liberty expression:\n%s\n", tree.str().c_str());

	if (tree.kind == LibertyExpression::Kind::EMPTY) {
		if (!warned_cells.count(cell_name)) {
			log_debug("Invalid expression '%s' in next_state attribute of cell '%s' - skipping.\n", expr.c_str(), cell_name.c_str());
			warned_cells.insert(cell_name);
		}
		return false;
	}

	auto pin_names = pool<std::string>{};
	tree.get_pin_names(pin_names);

	// from the `ff` block, we know the flop output signal name for loopback.
	auto ff = cell->find("ff");
	if (ff == nullptr || ff->args.size() != 2)
		return false;
	auto ff_output = ff->args.at(0);
	
	// This test is redundant with the one in enable_pin, but we're in a
	// position that gives better diagnostics here.
	if (!pin_names.count(ff_output)) {
		if (!warned_cells.count(cell_name)) {
			log_debug("Inference failed on expression '%s' in next_state attribute of cell '%s' because it does not contain ff output '%s' - skipping.\n", expr.c_str(), cell_name.c_str(), ff_output.c_str());
			warned_cells.insert(cell_name);
		}
		return false;
	}

	data_not_inverted = true;
	data_name = "";
	enable_not_inverted = true;
	enable_name = "";

	if (pin_names.size() == 3 && pin_names.count(ff_output)) {
		pin_names.erase(ff_output);
		auto pins = std::vector<std::string>(pin_names.begin(), pin_names.end());
		int lut = 0;
		for (int n = 0; n < 8; n++) {
			auto values = dict<std::string, bool>{};
			values.insert(std::make_pair(pins[0], (n & 1) == 1));
			values.insert(std::make_pair(pins[1], (n & 2) == 2));
			values.insert(std::make_pair(ff_output, (n & 4) == 4));
			if (tree.eval(values))
				lut |= 1 << n;
		}
		// the ff output Q is in a known bit location, so we now just have to compare the LUT mask to known values to find the enable pin and polarity.
		if (lut == 0xD8) {
			data_name = pins[1];
			enable_name = pins[0];	
			return true;
		}
		if (lut == 0xB8) {
			data_name = pins[0];
			enable_name = pins[1];	
			return true;
		}
		enable_not_inverted = false;
		if (lut == 0xE4) {
			data_name = pins[1];
			enable_name = pins[0];	
			return true;
		}
		if (lut == 0xE2) {
			data_name = pins[0];
			enable_name = pins[1];	
			return true;
		}
		// this does not match an enable flop.
	}

	if (!warned_cells.count(cell_name)) {
		log_debug("Inference failed on expression '%s' in next_state attribute of cell '%s' because it does not evaluate to an enable flop - skipping.\n", expr.c_str(), cell_name.c_str());
		warned_cells.insert(cell_name);
	}
	return false;
}

static bool parse_pin(const LibertyAst *cell, const LibertyAst *attr, std::string &pin_name, bool &pin_pol)
{
	if (cell == nullptr || attr == nullptr || attr->value.empty())
		return false;

	std::string value = attr->value;

	for (size_t pos = value.find_first_of("\" \t()"); pos != std::string::npos; pos = value.find_first_of("\" \t()"))
		value.erase(pos, 1);

	if (value[value.size()-1] == '\'') {
		pin_name = value.substr(0, value.size()-1);
		pin_pol = false;
	} else if (value[0] == '!') {
		pin_name = value.substr(1, value.size()-1);
		pin_pol = false;
	} else {
		pin_name = value;
		pin_pol = true;
	}

	for (auto child : cell->children)
		if (child->id == "pin" && child->args.size() == 1 && child->args[0] == pin_name)
			return true;

	/* If we end up here, the pin specified in the attribute does not exist, which is an error,
	   or, the attribute contains an expression which we do not yet support.
       For now, we'll simply produce a warning to let the user know something is up.
	*/
	if (pin_name.find_first_of("^*|&") == std::string::npos) {
		log_debug("Malformed liberty file - cannot find pin '%s' in cell '%s' - skipping.\n", pin_name.c_str(), cell->args[0].c_str());
	}
	else {
		log_debug("Found unsupported expression '%s' in pin attribute of cell '%s' - skipping.\n", pin_name.c_str(), cell->args[0].c_str());
	}

	return false;
}

static void find_cell(std::vector<const LibertyAst *> cells, IdString cell_type, bool clkpol, bool has_reset, bool rstpol, bool rstval, bool has_enable, bool enapol, std::vector<std::string> &dont_use_cells)
{
	const LibertyAst *best_cell = nullptr;
	std::map<std::string, char> best_cell_ports;
	int best_cell_pins = 0;
	bool best_cell_noninv = false;
	double best_cell_area = 0;

	for (auto cell : cells)
	{
		const LibertyAst *dn = cell->find("dont_use");
		if (dn != nullptr && dn->value == "true")
			continue;

		bool dont_use = false;
		for (std::string &dont_use_cell : dont_use_cells)
		{
			if (patmatch(dont_use_cell.c_str(), cell->args[0].c_str()))
			{
				dont_use = true;
				break;
			}
		}
		if (dont_use)
			continue;

		const LibertyAst *ff = cell->find("ff");
		if (ff == nullptr)
			continue;

		std::string cell_clk_pin, cell_rst_pin, cell_next_pin, cell_enable_pin;
		bool cell_clk_pol, cell_rst_pol, cell_next_pol, cell_enable_pol;

		if (!parse_pin(cell, ff->find("clocked_on"), cell_clk_pin, cell_clk_pol) || cell_clk_pol != clkpol)
			continue;
		if (!parse_next_state(cell, ff->find("next_state"), cell_next_pin, cell_next_pol, cell_enable_pin, cell_enable_pol) || (has_enable && (cell_enable_pin.empty() || cell_enable_pol != enapol)))
			continue;
		if (has_reset && rstval == false) {
			if (!parse_pin(cell, ff->find("clear"), cell_rst_pin, cell_rst_pol) || cell_rst_pol != rstpol)
				continue;
		}
		if (has_reset && rstval == true) {
			if (!parse_pin(cell, ff->find("preset"), cell_rst_pin, cell_rst_pol) || cell_rst_pol != rstpol)
				continue;
		}

		std::map<std::string, char> this_cell_ports;
		this_cell_ports[cell_clk_pin] = 'C';
		if (has_reset)
			this_cell_ports[cell_rst_pin] = 'R';
		if (has_enable)
			this_cell_ports[cell_enable_pin] = 'E';
		this_cell_ports[cell_next_pin] = 'D';

		double area = 0;
		const LibertyAst *ar = cell->find("area");
		if (ar != nullptr && !ar->value.empty())
			area = atof(ar->value.c_str());

		int num_pins = 0;
		bool found_output = false;
		bool found_noninv_output = false;
		for (auto pin : cell->children)
		{
			if (pin->id != "pin" || pin->args.size() != 1)
				continue;

			const LibertyAst *dir = pin->find("direction");
			if (dir == nullptr || dir->value == "internal")
				continue;
			num_pins++;

			if (dir->value == "input" && this_cell_ports.count(pin->args[0]) == 0)
				goto continue_cell_loop;

			const LibertyAst *func = pin->find("function");
			if (dir->value == "output" && func != nullptr) {
				std::string value = func->value;
				for (size_t pos = value.find_first_of("\" \t"); pos != std::string::npos; pos = value.find_first_of("\" \t"))
					value.erase(pos, 1);
				if (value == ff->args[0]) {
					this_cell_ports[pin->args[0]] = cell_next_pol ? 'Q' : 'q';
					if (cell_next_pol)
						found_noninv_output = true;
					found_output = true;
				} else
				if (value == ff->args[1]) {
					this_cell_ports[pin->args[0]] = cell_next_pol ? 'q' : 'Q';
					if (!cell_next_pol)
						found_noninv_output = true;
					found_output = true;
				}
			}

			if (this_cell_ports.count(pin->args[0]) == 0)
				this_cell_ports[pin->args[0]] = 0;
		}

		if (!found_output || (best_cell != nullptr && (num_pins > best_cell_pins || (best_cell_noninv && !found_noninv_output))))
			continue;

		if (best_cell != nullptr && num_pins == best_cell_pins && area > best_cell_area)
			continue;

		best_cell = cell;
		best_cell_pins = num_pins;
		best_cell_area = area;
		best_cell_noninv = found_noninv_output;
		best_cell_ports.swap(this_cell_ports);
	continue_cell_loop:;
	}

	if (best_cell != nullptr) {
		log("  cell %s (%sinv, pins=%d, area=%.2f) is a direct match for cell type %s.\n",
				best_cell->args[0].c_str(), best_cell_noninv ? "non" : "", best_cell_pins, best_cell_area, cell_type.c_str());
		cell_mappings[cell_type].cell_name = RTLIL::escape_id(best_cell->args[0]);
		cell_mappings[cell_type].ports = best_cell_ports;
	}
}

static void find_cell_sr(std::vector<const LibertyAst *> cells, IdString cell_type, bool clkpol, bool setpol, bool clrpol, bool has_enable, bool enapol, std::vector<std::string> &dont_use_cells)
{
	const LibertyAst *best_cell = nullptr;
	std::map<std::string, char> best_cell_ports;
	int best_cell_pins = 0;
	bool best_cell_noninv = false;
	double best_cell_area = 0;

	log_assert(!enapol && "set/reset cell with enable is unimplemented due to lack of cells for testing");

	for (auto cell : cells)
	{
		const LibertyAst *dn = cell->find("dont_use");
		if (dn != nullptr && dn->value == "true")
			continue;

		bool dont_use = false;
		for (std::string &dont_use_cell : dont_use_cells)
		{
			if (patmatch(dont_use_cell.c_str(), cell->args[0].c_str()))
			{
				dont_use = true;
				break;
			}
		}
		if (dont_use)
			continue;

		const LibertyAst *ff = cell->find("ff");
		if (ff == nullptr)
			continue;

		std::string cell_clk_pin, cell_set_pin, cell_clr_pin, cell_next_pin, cell_enable_pin;
		bool cell_clk_pol, cell_set_pol, cell_clr_pol, cell_next_pol, cell_enable_pol;

		if (!parse_pin(cell, ff->find("clocked_on"), cell_clk_pin, cell_clk_pol) || cell_clk_pol != clkpol)
			continue;
		if (!parse_next_state(cell, ff->find("next_state"), cell_next_pin, cell_next_pol, cell_enable_pin, cell_enable_pol))
			continue;

		if (!parse_pin(cell, ff->find("preset"), cell_set_pin, cell_set_pol))
			continue;
		if (!parse_pin(cell, ff->find("clear"), cell_clr_pin, cell_clr_pol))
			continue;
		if (!cell_next_pol) {
			// next_state is negated
			// we later propagate this inversion to the output,
			// which requires the swap of set and reset
			std::swap(cell_set_pin, cell_clr_pin);
			std::swap(cell_set_pol, cell_clr_pol);
		}
		if (cell_set_pol != setpol)
			continue;
		if (cell_clr_pol != clrpol)
			continue;

		std::map<std::string, char> this_cell_ports;
		this_cell_ports[cell_clk_pin] = 'C';
		this_cell_ports[cell_set_pin] = 'S';
		this_cell_ports[cell_clr_pin] = 'R';
		if (has_enable)
			this_cell_ports[cell_enable_pin] = 'E';
		this_cell_ports[cell_next_pin] = 'D';

		double area = 0;
		const LibertyAst *ar = cell->find("area");
		if (ar != nullptr && !ar->value.empty())
			area = atof(ar->value.c_str());

		int num_pins = 0;
		bool found_output = false;
		bool found_noninv_output = false;
		for (auto pin : cell->children)
		{
			if (pin->id != "pin" || pin->args.size() != 1)
				continue;

			const LibertyAst *dir = pin->find("direction");
			if (dir == nullptr || dir->value == "internal")
				continue;
			num_pins++;

			if (dir->value == "input" && this_cell_ports.count(pin->args[0]) == 0)
				goto continue_cell_loop;

			const LibertyAst *func = pin->find("function");
			if (dir->value == "output" && func != nullptr) {
				std::string value = func->value;
				for (size_t pos = value.find_first_of("\" \t"); pos != std::string::npos; pos = value.find_first_of("\" \t"))
					value.erase(pos, 1);
				if (value == ff->args[0]) {
					// next_state negation propagated to output
					this_cell_ports[pin->args[0]] = cell_next_pol ? 'Q' : 'q';
					if (cell_next_pol)
						found_noninv_output = true;
					found_output = true;
				} else
				if (value == ff->args[1]) {
					// next_state negation propagated to output
					this_cell_ports[pin->args[0]] = cell_next_pol ? 'q' : 'Q';
					if (!cell_next_pol)
						found_noninv_output = true;
					found_output = true;
				}
			}

			if (this_cell_ports.count(pin->args[0]) == 0)
				this_cell_ports[pin->args[0]] = 0;
		}

		if (!found_output || (best_cell != nullptr && (num_pins > best_cell_pins || (best_cell_noninv && !found_noninv_output))))
			continue;

		if (best_cell != nullptr && num_pins == best_cell_pins && area > best_cell_area)
			continue;

		best_cell = cell;
		best_cell_pins = num_pins;
		best_cell_area = area;
		best_cell_noninv = found_noninv_output;
		best_cell_ports.swap(this_cell_ports);
	continue_cell_loop:;
	}

	if (best_cell != nullptr) {
		log("  cell %s (%sinv, pins=%d, area=%.2f) is a direct match for cell type %s.\n",
				best_cell->args[0].c_str(), best_cell_noninv ? "non" : "", best_cell_pins, best_cell_area, cell_type.c_str());
		cell_mappings[cell_type].cell_name = RTLIL::escape_id(best_cell->args[0]);
		cell_mappings[cell_type].ports = best_cell_ports;
	}
}

static void dfflibmap(RTLIL::Design *design, RTLIL::Module *module)
{
	log("Mapping DFF cells in module `%s':\n", module->name.c_str());

	dict<SigBit, pool<Cell*>> notmap;
	SigMap sigmap(module);

	std::vector<RTLIL::Cell*> cell_list;
	for (auto cell : module->cells()) {
		if (design->selected(module, cell) && cell_mappings.count(cell->type) > 0)
			cell_list.push_back(cell);
		if (cell->type == ID($_NOT_))
			notmap[sigmap(cell->getPort(ID::A))].insert(cell);
	}

	std::map<std::string, int> stats;
	for (auto cell : cell_list)
	{
		auto cell_type = cell->type;
		auto cell_name = cell->name;
		auto cell_connections = cell->connections();
		std::string src = cell->get_src_attribute();

		module->remove(cell);

		cell_mapping &cm = cell_mappings[cell_type];
		RTLIL::Cell *new_cell = module->addCell(cell_name, cm.cell_name);

		new_cell->set_src_attribute(src);

		bool has_q = false, has_qn = false;
		for (auto &port : cm.ports) {
			if (port.second == 'Q') has_q = true;
			if (port.second == 'q') has_qn = true;
		}

		for (auto &port : cm.ports) {
			RTLIL::SigSpec sig;
			if ('A' <= port.second && port.second <= 'Z') {
				sig = cell_connections[std::string("\\") + port.second];
			} else
			if (port.second == 'q') {
				RTLIL::SigSpec old_sig = cell_connections[std::string("\\") + char(port.second - ('a' - 'A'))];
				sig = module->addWire(NEW_ID, GetSize(old_sig));
				if (has_q && has_qn) {
					for (auto &it : notmap[sigmap(old_sig)]) {
						module->connect(it->getPort(ID::Y), sig);
						it->setPort(ID::Y, module->addWire(NEW_ID, GetSize(old_sig)));
					}
				} else {
					module->addNotGate(NEW_ID, sig, old_sig);
				}
			} else
			if ('a' <= port.second && port.second <= 'z') {
				sig = cell_connections[std::string("\\") + char(port.second - ('a' - 'A'))];
				sig = module->NotGate(NEW_ID, sig);
			} else
			if (port.second == '0' || port.second == '1') {
				sig = RTLIL::SigSpec(port.second == '0' ? 0 : 1, 1);
			} else
			if (port.second == 0) {
				sig = module->addWire(NEW_ID);
			} else
				log_abort();
			new_cell->setPort("\\" + port.first, sig);
		}

		stats[stringf("  mapped %%d %s cells to %s cells.\n", cell_type.c_str(), new_cell->type.c_str())]++;
	}

	for (auto &stat: stats)
		log(stat.first.c_str(), stat.second);
}

struct DfflibmapPass : public Pass {
	DfflibmapPass() : Pass("dfflibmap", "technology mapping of flip-flops") { }
	void help() override
	{
		log("\n");
		log("    dfflibmap [-prepare] [-map-only] [-info] [-dont_use <cell_name>] -liberty <file> [selection]\n");
		log("\n");
		log("Map internal flip-flop cells to the flip-flop cells in the technology\n");
		log("library specified in the given liberty files.\n");
		log("\n");
		log("This pass may add inverters as needed. Therefore it is recommended to\n");
		log("first run this pass and then map the logic paths to the target technology.\n");
		log("\n");
		log("When called with -prepare, this command will convert the internal FF cells\n");
		log("to the internal cell types that best match the cells found in the given\n");
		log("liberty file, but won't actually map them to the target cells.\n");
		log("\n");
		log("When called with -map-only, this command will only map internal cell\n");
		log("types that are already of exactly the right type to match the target\n");
		log("cells, leaving remaining internal cells untouched.\n");
		log("\n");
		log("When called with -info, this command will only print the target cell\n");
		log("list, along with their associated internal cell types, and the arguments\n");
		log("that would be passed to the dfflegalize pass.  The design will not be\n");
		log("changed.\n");
		log("\n");
		log("When called with -dont_use, this command will not map to the specified cell\n");
		log("name as an alternative to setting the dont_use property in the Liberty file.\n");
		log("This argument can be called multiple times with different cell names. This\n");
		log("argument also supports simple glob patterns in the cell name.\n");
		log("\n");
	}
	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		log_header(design, "Executing DFFLIBMAP pass (mapping DFF cells to sequential cells from liberty file).\n");
		log_push();

		bool prepare_mode = false;
		bool map_only_mode = false;
		bool info_mode = false;

		std::vector<std::string> liberty_files;
		std::vector<std::string> dont_use_cells;

		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++)
		{
			std::string arg = args[argidx];
			if (arg == "-liberty" && argidx+1 < args.size()) {
				std::string liberty_file = args[++argidx];
				rewrite_filename(liberty_file);
				liberty_files.push_back(liberty_file);
				continue;
			}
			if (arg == "-prepare") {
				prepare_mode = true;
				continue;
			}
			if (arg == "-map-only") {
				map_only_mode = true;
				continue;
			}
			if (arg == "-info") {
				info_mode = true;
				continue;
			}
			if (arg == "-dont_use" && argidx+1 < args.size()) {
				dont_use_cells.push_back(args[++argidx]);
				continue;
			}
			break;
		}
		extra_args(args, argidx, design);

		int modes = 0;
		if (prepare_mode)
			modes++;
		if (map_only_mode)
			modes++;
		if (info_mode)
			modes++;
		if (modes > 1)
			log_cmd_error("Only one of -prepare, -map-only, or -info options should be given!\n");

		if (liberty_files.empty())
			log_cmd_error("Missing `-liberty liberty_file' option!\n");

		LibertyMergedCells merged;
		for (auto path : liberty_files) {
			std::istream* f = uncompressed(path);
			LibertyParser p(*f, path);
			merged.merge(p);
			delete f;
		}

		find_cell(merged.cells, ID($_DFF_N_), false, false, false, false, false, false, dont_use_cells);
		find_cell(merged.cells, ID($_DFF_P_), true, false, false, false, false, false, dont_use_cells);

		find_cell(merged.cells, ID($_DFF_NN0_), false, true, false, false, false, false, dont_use_cells);
		find_cell(merged.cells, ID($_DFF_NN1_), false, true, false, true, false, false, dont_use_cells);
		find_cell(merged.cells, ID($_DFF_NP0_), false, true, true, false, false, false, dont_use_cells);
		find_cell(merged.cells, ID($_DFF_NP1_), false, true, true, true, false, false, dont_use_cells);
		find_cell(merged.cells, ID($_DFF_PN0_), true, true, false, false, false, false, dont_use_cells);
		find_cell(merged.cells, ID($_DFF_PN1_), true, true, false, true, false, false, dont_use_cells);
		find_cell(merged.cells, ID($_DFF_PP0_), true, true, true, false, false, false, dont_use_cells);
		find_cell(merged.cells, ID($_DFF_PP1_), true, true, true, true, false, false, dont_use_cells);

		find_cell(merged.cells, ID($_DFFE_NN_), false, false, false, false, true, false, dont_use_cells);
		find_cell(merged.cells, ID($_DFFE_NP_), false, false, false, false, true, true, dont_use_cells);
		find_cell(merged.cells, ID($_DFFE_PN_), true, false, false, false, true, false, dont_use_cells);
		find_cell(merged.cells, ID($_DFFE_PP_), true, false, false, false, true, true, dont_use_cells);

		find_cell_sr(merged.cells, ID($_DFFSR_NNN_), false, false, false, false, false, dont_use_cells);
		find_cell_sr(merged.cells, ID($_DFFSR_NNP_), false, false, true, false, false, dont_use_cells);
		find_cell_sr(merged.cells, ID($_DFFSR_NPN_), false, true, false, false, false, dont_use_cells);
		find_cell_sr(merged.cells, ID($_DFFSR_NPP_), false, true, true, false, false, dont_use_cells);
		find_cell_sr(merged.cells, ID($_DFFSR_PNN_), true, false, false, false, false, dont_use_cells);
		find_cell_sr(merged.cells, ID($_DFFSR_PNP_), true, false, true, false, false, dont_use_cells);
		find_cell_sr(merged.cells, ID($_DFFSR_PPN_), true, true, false, false, false, dont_use_cells);
		find_cell_sr(merged.cells, ID($_DFFSR_PPP_), true, true, true, false, false, dont_use_cells);

		log("  final dff cell mappings:\n");
		logmap_all();

		if (!map_only_mode) {
			std::string dfflegalize_cmd = "dfflegalize";
			for (auto it : cell_mappings)
				dfflegalize_cmd += stringf(" -cell %s 01", it.first.c_str());
			dfflegalize_cmd += " t:$_DFF* t:$_SDFF*";
			if (info_mode) {
				log("dfflegalize command line: %s\n", dfflegalize_cmd.c_str());
			} else {
				Pass::call(design, dfflegalize_cmd);
			}
		}

		if (!prepare_mode && !info_mode) {
			for (auto module : design->selected_modules())
				if (!module->get_blackbox_attribute())
					dfflibmap(design, module);
		}

		log_pop();
		cell_mappings.clear();
	}
} DfflibmapPass;

PRIVATE_NAMESPACE_END
