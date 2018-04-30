#include "graph_utils.h"

#include <queue>

namespace celerity {

namespace graph_utils {
	task_vertices add_task(task_id tid, const task_dag& tdag, command_dag& cdag) {
		const vertex begin_task_v = boost::add_vertex(cdag);
		cdag[begin_task_v].label = (boost::format("Begin %s") % tdag[tid].label).str();
		cdag[begin_task_v].tid = tid;

		// Add all task requirements
		for_predecessors(tdag, static_cast<vertex>(tid), [&cdag, begin_task_v](vertex requirement) {
			boost::add_edge(cdag[boost::graph_bundle].task_complete_vertices[static_cast<task_id>(requirement)], begin_task_v, cdag);
		});

		const vertex complete_task_v = boost::add_vertex(cdag);
		cdag[boost::graph_bundle].task_complete_vertices[tid] = complete_task_v;
		cdag[complete_task_v].label = (boost::format("Complete %s") % tdag[tid].label).str();
		cdag[complete_task_v].tid = tid;

		return task_vertices(begin_task_v, complete_task_v);
	}

	vertex add_master_access_cmd(const task_vertices& tv, command_dag& cdag) {
		const node_id master_nid = 0;
		const auto v = boost::add_vertex(cdag);
		boost::add_edge(tv.first, v, cdag);
		boost::add_edge(v, tv.second, cdag);
		cdag[v].cmd = command::MASTER_ACCESS;
		cdag[v].nid = master_nid;
		cdag[v].tid = cdag[tv.first].tid;
		cdag[v].label = (boost::format("Node %d:\\nMASTER ACCESS") % master_nid).str();
		cdag[v].data.master_access = {};
		return v;
	}

	std::vector<task_id> get_satisfied_sibling_set(const task_dag& tdag) {
		for(auto v : tdag.vertex_set()) {
			if(tdag[v].processed || tdag[v].num_unsatisfied > 0) continue;

			std::unordered_set<task_id> checked_predecessors;
			std::unordered_set<task_id> candidates;
			std::queue<task_id> unchecked_siblings;
			candidates.insert(v);
			unchecked_siblings.push(v);

			bool abort = false;
			while(!abort && !unchecked_siblings.empty()) {
				const task_id sib = unchecked_siblings.front();
				unchecked_siblings.pop();

				abort = !for_predecessors(tdag, static_cast<vertex>(sib), [&](vertex pre) {
					if(!tdag[pre].processed) return false;
					auto pre_tid = static_cast<task_id>(pre);
					if(checked_predecessors.find(pre_tid) != checked_predecessors.end()) { return true; }
					checked_predecessors.insert(pre_tid);

					abort = !for_successors(tdag, pre, [&](vertex suc) {
						auto suc_tid = static_cast<task_id>(suc);
						if(candidates.find(suc_tid) == candidates.end()) {
							if(tdag[suc].processed || tdag[suc].num_unsatisfied > 0) { return false; }
							candidates.insert(suc_tid);
							unchecked_siblings.push(suc_tid);
						}
						return true;
					});

					// abort if v has unsatisfied sibling
					return !abort;
				});
			}

			if(!abort) {
				std::vector<task_id> result;
				result.insert(result.end(), candidates.begin(), candidates.end());
				return result;
			}
		}

		return std::vector<task_id>();
	}

	void mark_as_processed(task_id tid, task_dag& tdag) {
		tdag[tid].processed = true;
		for_successors(tdag, static_cast<vertex>(tid), [&tdag](vertex suc) {
			assert(tdag[suc].num_unsatisfied >= 1);
			tdag[suc].num_unsatisfied--;
		});
	}


	// --------------------------- Graph printing ---------------------------


	void print_graph(const celerity::task_dag& tdag, std::shared_ptr<logger> graph_logger) {
		write_graph_mux(tdag, boost::make_label_writer(boost::get(&celerity::tdag_vertex_properties::label, tdag)), boost::default_writer(), graph_logger);
	}

	void print_graph(const celerity::command_dag& cdag, std::shared_ptr<logger> graph_logger) {
		write_graph_mux(cdag,
		    [&](std::ostream& out, vertex v) {
			    const char* colors[] = {"black", "crimson", "dodgerblue4", "goldenrod", "maroon4", "springgreen2", "tan1", "chartreuse2"};

			    std::unordered_map<std::string, std::string> props;
			    props["label"] = boost::escape_dot_string(cdag[v].label);

			    props["fontcolor"] = colors[cdag[v].nid % (sizeof(colors) / sizeof(char*))];

			    switch(cdag[v].cmd) {
			    case command::NOP:
				    props["color"] = "gray50";
				    props["fontcolor"] = "gray50";
				    break;
			    case command::COMPUTE: props["shape"] = "box"; break;
			    default: break;
			    }

			    out << "[";
			    for(auto it : props) {
				    out << " " << it.first << "=" << it.second;
			    }
			    out << "]";
		    },
		    [&](std::ostream& out, auto e) {
			    vertex v0 = boost::source(e, cdag);
			    vertex v1 = boost::target(e, cdag);
			    if((cdag[v0].cmd == command::PULL || cdag[v0].cmd == command::AWAIT_PULL)
			        && (cdag[v1].cmd == command::PULL || cdag[v1].cmd == command::AWAIT_PULL)) {
				    out << "[color=gray50]";
			    }
		    },
		    graph_logger);
	}

} // namespace graph_utils

} // namespace celerity
