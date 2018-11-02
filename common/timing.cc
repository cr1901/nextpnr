/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2018  David Shah <david@symbioticeda.com>
 *  Copyright (C) 2018  Eddie Hung <eddieh@ece.ubc.ca>
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

#include "timing.h"
#include <algorithm>
#include <boost/range/adaptor/reversed.hpp>
#include <deque>
#include <map>
#include <unordered_map>
#include <utility>
#include "log.h"
#include "util.h"

NEXTPNR_NAMESPACE_BEGIN

namespace {
struct ClockEvent
{
    IdString clock;
    ClockEdge edge;

    bool operator==(const ClockEvent &other) const { return clock == other.clock && edge == other.edge; }
};

struct ClockPair
{
    ClockEvent start, end;

    bool operator==(const ClockPair &other) const { return start == other.start && end == other.end; }
};
} // namespace

NEXTPNR_NAMESPACE_END
namespace std {

template <> struct hash<NEXTPNR_NAMESPACE_PREFIX ClockEvent>
{
    std::size_t operator()(const NEXTPNR_NAMESPACE_PREFIX ClockEvent &obj) const noexcept
    {
        std::size_t seed = 0;
        boost::hash_combine(seed, hash<NEXTPNR_NAMESPACE_PREFIX IdString>()(obj.clock));
        boost::hash_combine(seed, hash<int>()(int(obj.edge)));
        return seed;
    }
};

template <> struct hash<NEXTPNR_NAMESPACE_PREFIX ClockPair>
{
    std::size_t operator()(const NEXTPNR_NAMESPACE_PREFIX ClockPair &obj) const noexcept
    {
        std::size_t seed = 0;
        boost::hash_combine(seed, hash<NEXTPNR_NAMESPACE_PREFIX ClockEvent>()(obj.start));
        boost::hash_combine(seed, hash<NEXTPNR_NAMESPACE_PREFIX ClockEvent>()(obj.start));
        return seed;
    }
};

} // namespace std
NEXTPNR_NAMESPACE_BEGIN

typedef std::vector<const PortRef *> PortRefVector;
typedef std::map<int, unsigned> DelayFrequency;

struct CriticalPath
{
    PortRefVector ports;
    delay_t path_delay;
    delay_t path_period;
};

typedef std::unordered_map<ClockPair, CriticalPath> CriticalPathMap;

struct Timing
{
    Context *ctx;
    bool net_delays;
    bool update;
    delay_t min_slack;
    CriticalPathMap *crit_path;
    DelayFrequency *slack_histogram;
    IdString async_clock;

    struct TimingData
    {
        TimingData() : max_arrival(), max_path_length(), min_remaining_budget() {}
        TimingData(delay_t max_arrival) : max_arrival(max_arrival), max_path_length(), min_remaining_budget() {}
        delay_t max_arrival;
        unsigned max_path_length = 0;
        delay_t min_remaining_budget;
        bool false_startpoint = false;
        std::unordered_map<ClockEvent, delay_t> arrival_time;
    };

    Timing(Context *ctx, bool net_delays, bool update, CriticalPathMap *crit_path = nullptr,
           DelayFrequency *slack_histogram = nullptr)
            : ctx(ctx), net_delays(net_delays), update(update), min_slack(1.0e12 / ctx->target_freq),
              crit_path(crit_path), slack_histogram(slack_histogram), async_clock(ctx->id("$async$"))
    {
    }

    delay_t walk_paths()
    {
        const auto clk_period = delay_t(1.0e12 / ctx->target_freq);

        // First, compute the topographical order of nets to walk through the circuit, assuming it is a _acyclic_ graph
        // TODO(eddieh): Handle the case where it is cyclic, e.g. combinatorial loops
        std::vector<NetInfo *> topographical_order;
        std::unordered_map<const NetInfo *, std::unordered_map<ClockEvent, TimingData>> net_data;
        // In lieu of deleting edges from the graph, simply count the number of fanins to each output port
        std::unordered_map<const PortInfo *, unsigned> port_fanin;

        std::vector<IdString> input_ports;
        std::vector<const PortInfo *> output_ports;
        for (auto &cell : ctx->cells) {
            input_ports.clear();
            output_ports.clear();
            for (auto &port : cell.second->ports) {
                if (!port.second.net)
                    continue;
                if (port.second.type == PORT_OUT)
                    output_ports.push_back(&port.second);
                else
                    input_ports.push_back(port.first);
            }

            for (auto o : output_ports) {
                int clocks = 0;
                TimingPortClass portClass = ctx->getPortTimingClass(cell.second.get(), o->name, clocks);
                // If output port is influenced by a clock (e.g. FF output) then add it to the ordering as a timing
                // start-point
                if (portClass == TMG_REGISTER_OUTPUT) {
                    topographical_order.emplace_back(o->net);
                    for (int i = 0; i < clocks; i++) {
                        TimingClockingInfo clkInfo = ctx->getPortClockingInfo(cell.second.get(), o->name, i);
                        const NetInfo *clknet = get_net_or_empty(cell.second.get(), clkInfo.clock_port);
                        IdString clksig = clknet ? clknet->name : async_clock;
                        net_data[o->net][ClockEvent{clksig, clknet ? clkInfo.edge : RISING_EDGE}] =
                                TimingData{clkInfo.clockToQ.maxDelay()};
                    }

                } else {
                    if (portClass == TMG_STARTPOINT || portClass == TMG_GEN_CLOCK || portClass == TMG_IGNORE) {
                        topographical_order.emplace_back(o->net);
                        TimingData td;
                        td.false_startpoint = (portClass == TMG_GEN_CLOCK || portClass == TMG_IGNORE);
                        net_data[o->net][ClockEvent{async_clock, RISING_EDGE}] = td;
                    }
                    // Otherwise, for all driven input ports on this cell, if a timing arc exists between the input and
                    // the current output port, increment fanin counter
                    for (auto i : input_ports) {
                        DelayInfo comb_delay;
                        bool is_path = ctx->getCellDelay(cell.second.get(), i, o->name, comb_delay);
                        if (is_path)
                            port_fanin[o]++;
                    }
                }
            }
        }

        std::deque<NetInfo *> queue(topographical_order.begin(), topographical_order.end());

        // Now walk the design, from the start points identified previously, building up a topographical order
        while (!queue.empty()) {
            const auto net = queue.front();
            queue.pop_front();

            for (auto &usr : net->users) {
                int user_clocks;
                TimingPortClass usrClass = ctx->getPortTimingClass(usr.cell, usr.port, user_clocks);
                if (usrClass == TMG_IGNORE || usrClass == TMG_CLOCK_INPUT)
                    continue;
                for (auto &port : usr.cell->ports) {
                    if (port.second.type != PORT_OUT || !port.second.net)
                        continue;
                    int port_clocks;
                    TimingPortClass portClass = ctx->getPortTimingClass(usr.cell, port.first, port_clocks);

                    // Skip if this is a clocked output (but allow non-clocked ones)
                    if (portClass == TMG_REGISTER_OUTPUT || portClass == TMG_STARTPOINT || portClass == TMG_IGNORE ||
                        portClass == TMG_GEN_CLOCK)
                        continue;
                    DelayInfo comb_delay;
                    bool is_path = ctx->getCellDelay(usr.cell, usr.port, port.first, comb_delay);
                    if (!is_path)
                        continue;
                    // Decrement the fanin count, and only add to topographical order if all its fanins have already
                    // been visited
                    auto it = port_fanin.find(&port.second);
                    NPNR_ASSERT(it != port_fanin.end());
                    if (--it->second == 0) {
                        topographical_order.emplace_back(port.second.net);
                        queue.emplace_back(port.second.net);
                        port_fanin.erase(it);
                    }
                }
            }
        }

        // Sanity check to ensure that all ports where fanins were recorded were indeed visited
        if (!port_fanin.empty()) {
            for (auto fanin : port_fanin) {
                NetInfo *net = fanin.first->net;
                if (net != nullptr) {
                    log_info("   remaining fanin includes %s (net %s)\n", fanin.first->name.c_str(ctx),
                             net->name.c_str(ctx));
                    if (net->driver.cell != nullptr)
                        log_info("        driver = %s.%s\n", net->driver.cell->name.c_str(ctx),
                                 net->driver.port.c_str(ctx));
                    for (auto net_user : net->users)
                        log_info("        user: %s.%s\n", net_user.cell->name.c_str(ctx), net_user.port.c_str(ctx));
                } else {
                    log_info("   remaining fanin includes %s (no net)\n", fanin.first->name.c_str(ctx));
                }
            }
        }
        NPNR_ASSERT(port_fanin.empty());

        // Go forwards topographically to find the maximum arrival time and max path length for each net
        for (auto net : topographical_order) {
            auto &nd_map = net_data.at(net);
            for (auto &startdomain : nd_map) {
                ClockEvent start_clk = startdomain.first;
                auto &nd = startdomain.second;
                const auto net_arrival = nd.max_arrival;
                const auto net_length_plus_one = nd.max_path_length + 1;
                nd.min_remaining_budget = clk_period;
                for (auto &usr : net->users) {
                    int port_clocks;
                    TimingPortClass portClass = ctx->getPortTimingClass(usr.cell, usr.port, port_clocks);
                    auto net_delay = net_delays ? ctx->getNetinfoRouteDelay(net, usr) : delay_t();
                    auto usr_arrival = net_arrival + net_delay;

                    if (portClass == TMG_REGISTER_INPUT || portClass == TMG_ENDPOINT || portClass == TMG_IGNORE) {
                        // Skip
                    } else {
                        auto budget_override = ctx->getBudgetOverride(net, usr, net_delay);
                        // Iterate over all output ports on the same cell as the sink
                        for (auto port : usr.cell->ports) {
                            if (port.second.type != PORT_OUT || !port.second.net)
                                continue;
                            DelayInfo comb_delay;
                            // Look up delay through this path
                            bool is_path = ctx->getCellDelay(usr.cell, usr.port, port.first, comb_delay);
                            if (!is_path)
                                continue;
                            auto &data = net_data[port.second.net][start_clk];
                            auto &arrival = data.max_arrival;
                            arrival = std::max(arrival, usr_arrival + comb_delay.maxDelay());
                            if (!budget_override) { // Do not increment path length if budget overriden since it doesn't
                                // require a share of the slack
                                auto &path_length = data.max_path_length;
                                path_length = std::max(path_length, net_length_plus_one);
                            }
                        }
                    }
                }
            }
        }

        std::unordered_map<ClockPair, std::pair<delay_t, NetInfo *>> crit_nets;

        // Now go backwards topographically to determine the minimum path slack, and to distribute all path slack evenly
        // between all nets on the path
        for (auto net : boost::adaptors::reverse(topographical_order)) {
            auto &nd_map = net_data.at(net);
            for (auto &startdomain : nd_map) {
                auto &nd = startdomain.second;
                // Ignore false startpoints
                if (nd.false_startpoint)
                    continue;
                const delay_t net_length_plus_one = nd.max_path_length + 1;
                auto &net_min_remaining_budget = nd.min_remaining_budget;
                for (auto &usr : net->users) {
                    auto net_delay = net_delays ? ctx->getNetinfoRouteDelay(net, usr) : delay_t();
                    auto budget_override = ctx->getBudgetOverride(net, usr, net_delay);
                    int port_clocks;
                    TimingPortClass portClass = ctx->getPortTimingClass(usr.cell, usr.port, port_clocks);
                    if (portClass == TMG_REGISTER_INPUT || portClass == TMG_ENDPOINT) {
                        auto process_endpoint = [&](IdString clksig, ClockEdge edge, delay_t setup) {
                            const auto net_arrival = nd.max_arrival;
                            const auto endpoint_arrival = net_arrival + net_delay + setup;
                            auto path_budget = clk_period - endpoint_arrival;
                            delay_t period;

                            if (edge == startdomain.first.edge) {
                                period = clk_period;
                            } else {
                                period = clk_period / 2;
                            }

                            if (update) {
                                auto budget_share = budget_override ? 0 : path_budget / net_length_plus_one;
                                usr.budget = std::min(usr.budget, net_delay + budget_share);
                                net_min_remaining_budget =
                                        std::min(net_min_remaining_budget, path_budget - budget_share);
                            }

                            if (path_budget < min_slack)
                                min_slack = path_budget;

                            if (slack_histogram) {
                                int slack_ps = ctx->getDelayNS(path_budget) * 1000;
                                (*slack_histogram)[slack_ps]++;
                            }
                            ClockEvent dest_ev{clksig, edge};
                            ClockPair clockPair{startdomain.first, dest_ev};
                            nd.arrival_time[dest_ev] = std::max(nd.arrival_time[dest_ev], endpoint_arrival);

                            if (crit_path) {
                                if (!crit_nets.count(clockPair) || crit_nets.at(clockPair).first < endpoint_arrival) {
                                    crit_nets[clockPair] = std::make_pair(endpoint_arrival, net);
                                    (*crit_path)[clockPair].path_delay = endpoint_arrival;
                                    (*crit_path)[clockPair].path_period = clk_period;
                                    (*crit_path)[clockPair].ports.clear();
                                    (*crit_path)[clockPair].ports.push_back(&usr);
                                }
                            }
                        };
                        if (portClass == TMG_REGISTER_INPUT) {
                            for (int i = 0; i < port_clocks; i++) {
                                TimingClockingInfo clkInfo = ctx->getPortClockingInfo(usr.cell, usr.port, i);
                                const NetInfo *clknet = get_net_or_empty(usr.cell, clkInfo.clock_port);
                                IdString clksig = clknet ? clknet->name : async_clock;
                                process_endpoint(clksig, clknet ? RISING_EDGE : clkInfo.edge, clkInfo.setup.maxDelay());
                            }
                        } else {
                            process_endpoint(async_clock, RISING_EDGE, 0);
                        }

                    } else if (update) {

                        // Iterate over all output ports on the same cell as the sink
                        for (const auto &port : usr.cell->ports) {
                            if (port.second.type != PORT_OUT || !port.second.net)
                                continue;
                            DelayInfo comb_delay;
                            bool is_path = ctx->getCellDelay(usr.cell, usr.port, port.first, comb_delay);
                            if (!is_path)
                                continue;
                            if (net_data.count(port.second.net) &&
                                net_data.at(port.second.net).count(startdomain.first)) {
                                auto path_budget =
                                        net_data.at(port.second.net).at(startdomain.first).min_remaining_budget;
                                auto budget_share = budget_override ? 0 : path_budget / net_length_plus_one;
                                usr.budget = std::min(usr.budget, net_delay + budget_share);
                                net_min_remaining_budget =
                                        std::min(net_min_remaining_budget, path_budget - budget_share);
                            }
                        }
                    }
                }
            }
        }

        if (crit_path) {
            // Walk backwards from the most critical net
            for (auto crit_pair : crit_nets) {
                NetInfo *crit_net = crit_pair.second.second;
                auto &cp_ports = (*crit_path)[crit_pair.first].ports;
                while (crit_net) {
                    const PortInfo *crit_ipin = nullptr;
                    delay_t max_arrival = std::numeric_limits<delay_t>::min();

                    // Look at all input ports on its driving cell
                    for (const auto &port : crit_net->driver.cell->ports) {
                        if (port.second.type != PORT_IN || !port.second.net)
                            continue;
                        DelayInfo comb_delay;
                        bool is_path =
                                ctx->getCellDelay(crit_net->driver.cell, port.first, crit_net->driver.port, comb_delay);
                        if (!is_path)
                            continue;
                        // If input port is influenced by a clock, skip
                        int port_clocks;
                        TimingPortClass portClass =
                                ctx->getPortTimingClass(crit_net->driver.cell, port.first, port_clocks);
                        if (portClass == TMG_REGISTER_INPUT || portClass == TMG_CLOCK_INPUT ||
                            portClass == TMG_ENDPOINT || portClass == TMG_IGNORE)
                            continue;

                        // And find the fanin net with the latest arrival time
                        if (net_data.at(port.second.net).count(crit_pair.first.start)) {
                            const auto net_arrival = net_data.at(port.second.net).at(crit_pair.first.start).max_arrival;
                            if (net_arrival > max_arrival) {
                                max_arrival = net_arrival;
                                crit_ipin = &port.second;
                            }
                        }

                    }

                    if (!crit_ipin)
                        break;

                    // Now convert PortInfo* into a PortRef*
                    for (auto &usr : crit_ipin->net->users) {
                        if (usr.cell->name == crit_net->driver.cell->name && usr.port == crit_ipin->name) {
                            cp_ports.push_back(&usr);
                            break;
                        }
                    }
                    crit_net = crit_ipin->net;
                }
                std::reverse(cp_ports.begin(), cp_ports.end());
            }
        }
        return min_slack;
    }

    void assign_budget()
    {
        // Clear delays to a very high value first
        for (auto &net : ctx->nets) {
            for (auto &usr : net.second->users) {
                usr.budget = std::numeric_limits<delay_t>::max();
            }
        }

        walk_paths();
    }
};

void assign_budget(Context *ctx, bool quiet)
{
    if (!quiet) {
        log_break();
        log_info("Annotating ports with timing budgets for target frequency %.2f MHz\n", ctx->target_freq / 1e6);
    }

    Timing timing(ctx, ctx->slack_redist_iter > 0 /* net_delays */, true /* update */);
    timing.assign_budget();

    if (!quiet || ctx->verbose) {
        for (auto &net : ctx->nets) {
            for (auto &user : net.second->users) {
                // Post-update check
                if (!ctx->auto_freq && user.budget < 0)
                    log_warning("port %s.%s, connected to net '%s', has negative "
                                "timing budget of %fns\n",
                                user.cell->name.c_str(ctx), user.port.c_str(ctx), net.first.c_str(ctx),
                                ctx->getDelayNS(user.budget));
                else if (ctx->verbose)
                    log_info("port %s.%s, connected to net '%s', has "
                             "timing budget of %fns\n",
                             user.cell->name.c_str(ctx), user.port.c_str(ctx), net.first.c_str(ctx),
                             ctx->getDelayNS(user.budget));
            }
        }
    }

    // For slack redistribution, if user has not specified a frequency dynamically adjust the target frequency to be the
    // currently achieved maximum
    if (ctx->auto_freq && ctx->slack_redist_iter > 0) {
        delay_t default_slack = delay_t((1.0e9 / ctx->getDelayNS(1)) / ctx->target_freq);
        ctx->target_freq = 1.0e9 / ctx->getDelayNS(default_slack - timing.min_slack);
        if (ctx->verbose)
            log_info("minimum slack for this assign = %.2f ns, target Fmax for next "
                     "update = %.2f MHz\n",
                     ctx->getDelayNS(timing.min_slack), ctx->target_freq / 1e6);
    }

    if (!quiet)
        log_info("Checksum: 0x%08x\n", ctx->checksum());
}

void timing_analysis(Context *ctx, bool print_histogram, bool print_path)
{
    CriticalPathMap crit_paths;
    DelayFrequency slack_histogram;

    Timing timing(ctx, true /* net_delays */, false /* update */, print_path ? &crit_paths : nullptr,
                  print_histogram ? &slack_histogram : nullptr);
    auto min_slack = timing.walk_paths();

    if (print_path) {
        std::map<IdString, std::pair<ClockPair, CriticalPath>> clock_reports;
        for (auto path : crit_paths) {
            const ClockEvent &a = path.first.start;
            const ClockEvent &b = path.first.end;
            if (a.clock != b.clock || a.clock == ctx->id("$async$"))
                continue;
            delay_t slack = path.second.path_period - path.second.path_delay;
            if (!clock_reports.count(a.clock) ||
                slack < (clock_reports.at(a.clock).second.path_period - clock_reports.at(a.clock).second.path_delay)) {
                clock_reports[a.clock] = path;
            }
        }
        if (clock_reports.empty()) {
            log_warning("No clocks found in design");
        } else {
            delay_t total = 0;
            log_break();
            for (auto &clock : clock_reports) {
                log_info("Critical path report for clock '%s':\n", clock.first.c_str(ctx));
                log_info("curr total\n");
                auto &crit_path = clock.second.second.ports;
                auto &front = crit_path.front();
                auto &front_port = front->cell->ports.at(front->port);
                auto &front_driver = front_port.net->driver;

                int port_clocks;
                ctx->getPortTimingClass(front_driver.cell, front_driver.port, port_clocks);
                for (int i = 0; i < port_clocks; i++) {
                    TimingClockingInfo clockInfo = ctx->getPortClockingInfo(front_driver.cell, front_driver.port, i);
                    const NetInfo *clknet = get_net_or_empty(front_driver.cell, clockInfo.clock_port);
                    if (clknet != nullptr && clknet->name == clock.first &&
                        clockInfo.edge == clock.second.first.start.edge) {
                        IdString last_port = clockInfo.clock_port;

                        for (auto sink : crit_path) {
                            auto sink_cell = sink->cell;
                            auto &port = sink_cell->ports.at(sink->port);
                            auto net = port.net;
                            auto &driver = net->driver;
                            auto driver_cell = driver.cell;
                            DelayInfo comb_delay;
                            ctx->getCellDelay(sink_cell, last_port, driver.port, comb_delay);
                            total += comb_delay.maxDelay();
                            log_info("%4.1f %4.1f  Source %s.%s\n", ctx->getDelayNS(comb_delay.maxDelay()),
                                     ctx->getDelayNS(total), driver_cell->name.c_str(ctx), driver.port.c_str(ctx));
                            auto net_delay = ctx->getNetinfoRouteDelay(net, *sink);
                            total += net_delay;
                            auto driver_loc = ctx->getBelLocation(driver_cell->bel);
                            auto sink_loc = ctx->getBelLocation(sink_cell->bel);
                            log_info("%4.1f %4.1f    Net %s budget %f ns (%d,%d) -> (%d,%d)\n",
                                     ctx->getDelayNS(net_delay), ctx->getDelayNS(total), net->name.c_str(ctx),
                                     ctx->getDelayNS(sink->budget), driver_loc.x, driver_loc.y, sink_loc.x, sink_loc.y);
                            log_info("                Sink %s.%s\n", sink_cell->name.c_str(ctx), sink->port.c_str(ctx));
                            last_port = sink->port;
                        }
                    }
                }
                log_break();
            }
            log_break();
            for (auto &clock : clock_reports) {

                double Fmax;
                if (clock.second.first.start.edge == clock.second.first.end.edge)
                    Fmax = 1000 / ctx->getDelayNS(clock.second.second.path_delay);
                else
                    Fmax = 500 / ctx->getDelayNS(clock.second.second.path_delay);
                log_info("Max frequency for clock '%s': %.02f MHz\n", clock.first.c_str(ctx), Fmax);
            }
            log_break();
        }
    }

    if (print_histogram && slack_histogram.size() > 0) {
        unsigned num_bins = 20;
        unsigned bar_width = 60;
        auto min_slack = slack_histogram.begin()->first;
        auto max_slack = slack_histogram.rbegin()->first;
        auto bin_size = std::max(1u, (max_slack - min_slack) / num_bins);
        num_bins = std::min((max_slack - min_slack) / bin_size, num_bins) + 1;
        std::vector<unsigned> bins(num_bins);
        unsigned max_freq = 0;
        for (const auto &i : slack_histogram) {
            auto &bin = bins[(i.first - min_slack) / bin_size];
            bin += i.second;
            max_freq = std::max(max_freq, bin);
        }
        bar_width = std::min(bar_width, max_freq);

        log_break();
        log_info("Slack histogram:\n");
        log_info(" legend: * represents %d endpoint(s)\n", max_freq / bar_width);
        log_info("         + represents [1,%d) endpoint(s)\n", max_freq / bar_width);
        for (unsigned i = 0; i < num_bins; ++i)
            log_info("[%6d, %6d) |%s%c\n", min_slack + bin_size * i, min_slack + bin_size * (i + 1),
                     std::string(bins[i] * bar_width / max_freq, '*').c_str(),
                     (bins[i] * bar_width) % max_freq > 0 ? '+' : ' ');
    }
}

NEXTPNR_NAMESPACE_END
