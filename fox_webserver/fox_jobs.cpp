/*
	Copyright (C) 2024-2026 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.

	OrangeFox is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	OrangeFox is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <cstring>
#include <deque>
#include <memory>
#include <pthread.h>

#include "fox_json.hpp"
#include "fox_jobs.hpp"

namespace {

struct Job {
	int id = 0;
	Fox_Jobs::State state = Fox_Jobs::PENDING;
	int exit_code = -1;
	Fox_Rpc_Request request;
	std::string output;
	time_t created = 0;
	time_t finished = 0;
};

pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
std::deque<std::shared_ptr<Job>> g_jobs;  // bounded history, oldest at front
int g_next_id = 1;
int g_active_id = 0;  // RUNNING job id, 0 if idle

// Keep memory bounded: cap the number of retained jobs. Finished jobs are
// evicted oldest-first; a running/pending job is never evicted.
const size_t kMaxJobs = 64;

std::shared_ptr<Job> find_locked(int id) {
	for (auto& j : g_jobs)
		if (j->id == id)
			return j;
	return nullptr;
}

void prune_locked() {
	while (g_jobs.size() > kMaxJobs) {
		// Evict the oldest job that is finished and not the active one.
		bool removed = false;
		for (auto it = g_jobs.begin(); it != g_jobs.end(); ++it) {
			if ((*it)->state == Fox_Jobs::DONE && (*it)->id != g_active_id) {
				g_jobs.erase(it);
				removed = true;
				break;
			}
		}
		if (!removed)
			break;  // nothing evictable
	}
}

// funopen() write callback: append mirrored gui_print output to the job that
// owns this stream. The cookie is the raw Job*; its lifetime is guaranteed by
// the shared_ptr held in g_jobs while the job is RUNNING (a DONE job keeps its
// buffer too). bionic provides BSD funopen() rather than GNU fopencookie().
int cookie_write(void* cookie, const char* buf, int size) {
	Job* job = static_cast<Job*>(cookie);
	pthread_mutex_lock(&g_lock);
	job->output.append(buf, (size_t)size);
	pthread_mutex_unlock(&g_lock);
	return size;
}

int cookie_close(void* /*cookie*/) {
	// The Job outlives the FILE; nothing to release here.
	return 0;
}

const char* state_name(Fox_Jobs::State s) {
	switch (s) {
		case Fox_Jobs::PENDING: return "pending";
		case Fox_Jobs::RUNNING: return "running";
		default:                return "done";
	}
}

}  // namespace

int Fox_Jobs::Submit(const Fox_Rpc_Request& request) {
	if (!request.ok() || request.op.empty())
		return 0;
	pthread_mutex_lock(&g_lock);
	auto job = std::make_shared<Job>();
	job->id = g_next_id++;
	job->request = request;
	job->created = time(nullptr);
	g_jobs.push_back(job);
	prune_locked();
	int id = job->id;
	pthread_mutex_unlock(&g_lock);
	return id;
}

bool Fox_Jobs::Get(int id, Snapshot& out) {
	pthread_mutex_lock(&g_lock);
	auto job = find_locked(id);
	bool ok = false;
	if (job) {
		out.id = job->id;
		out.state = job->state;
		out.exit_code = job->exit_code;
		out.request = job->request;
		out.output_size = job->output.size();
		out.created = job->created;
		out.finished = job->finished;
		ok = true;
	}
	pthread_mutex_unlock(&g_lock);
	return ok;
}

bool Fox_Jobs::ReadOutput(int id, size_t& offset, std::string& out) {
	pthread_mutex_lock(&g_lock);
	auto job = find_locked(id);
	bool ok = false;
	if (job) {
		if (offset < job->output.size())
			out.append(job->output, offset, std::string::npos);
		offset = job->output.size();
		ok = true;
	}
	pthread_mutex_unlock(&g_lock);
	return ok;
}

bool Fox_Jobs::ReadOutputAndSnapshot(int id, size_t& offset, std::string& out,
                                     Snapshot& snap) {
	pthread_mutex_lock(&g_lock);
	auto job = find_locked(id);
	bool ok = false;
	if (job) {
		if (offset < job->output.size())
			out.append(job->output, offset, std::string::npos);
		offset = job->output.size();
		snap.id = job->id;
		snap.state = job->state;
		snap.exit_code = job->exit_code;
		snap.request = job->request;
		snap.output_size = job->output.size();
		snap.created = job->created;
		snap.finished = job->finished;
		ok = true;
	}
	pthread_mutex_unlock(&g_lock);
	return ok;
}

std::string Fox_Jobs::ListJson() {
	pthread_mutex_lock(&g_lock);
	Fox_Json_Array out;
	// Newest first.
	for (auto it = g_jobs.rbegin(); it != g_jobs.rend(); ++it) {
		Job* j = it->get();
		Fox_Json_Object job;
		job.AddInt("id", j->id)
		   .AddString("state", state_name(j->state))
		   .AddInt("exit_code", j->exit_code)
		   .AddSize("output_size", j->output.size())
		   .AddInt("created", (long long)j->created)
		   .AddInt("finished", (long long)j->finished)
		   .AddString("op", j->request.op);
		if (!j->request.id.empty())
			job.AddString("request_id", j->request.id);
		out.AddRaw(job.String());
	}
	std::string json = out.String();
	pthread_mutex_unlock(&g_lock);
	return json;
}

bool Fox_Jobs::HasPending() {
	pthread_mutex_lock(&g_lock);
	bool pending = false;
	if (g_active_id == 0) {
		for (auto& j : g_jobs) {
			if (j->state == PENDING) {
				pending = true;
				break;
			}
		}
	}
	pthread_mutex_unlock(&g_lock);
	return pending;
}

bool Fox_Jobs::BeginNext(FILE** out_file, Fox_Rpc_Request& out_request) {
	pthread_mutex_lock(&g_lock);
	if (g_active_id != 0) {
		pthread_mutex_unlock(&g_lock);
		return false;
	}
	std::shared_ptr<Job> job;
	for (auto& j : g_jobs) {
		if (j->state == PENDING) {
			job = j;
			break;
		}
	}
	if (!job) {
		pthread_mutex_unlock(&g_lock);
		return false;
	}
	job->state = RUNNING;
	g_active_id = job->id;
	out_request = job->request;
	pthread_mutex_unlock(&g_lock);

	FILE* f = funopen(job.get(), /*readfn=*/nullptr, cookie_write,
	                  /*seekfn=*/nullptr, cookie_close);
	if (!f) {
		pthread_mutex_lock(&g_lock);
		if (g_active_id == job->id) {
			job->state = PENDING;
			g_active_id = 0;
		}
		pthread_mutex_unlock(&g_lock);
		return false;
	}
	setvbuf(f, nullptr, _IONBF, 0);  // unbuffered: flush-per-write semantics
	*out_file = f;
	return true;
}

void Fox_Jobs::NotifyExit(int code) {
	pthread_mutex_lock(&g_lock);
	if (g_active_id != 0) {
		auto job = find_locked(g_active_id);
		if (job)
			job->exit_code = code;
	}
	pthread_mutex_unlock(&g_lock);
}

void Fox_Jobs::FinishActive() {
	pthread_mutex_lock(&g_lock);
	if (g_active_id != 0) {
		auto job = find_locked(g_active_id);
		if (job) {
			job->state = DONE;
			job->finished = time(nullptr);
		}
		g_active_id = 0;
	}
	pthread_mutex_unlock(&g_lock);
}

int Fox_Jobs::Active() {
	pthread_mutex_lock(&g_lock);
	int id = g_active_id;
	pthread_mutex_unlock(&g_lock);
	return id;
}
