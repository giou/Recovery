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

#ifndef _FOX_JOBS_HPP
#define _FOX_JOBS_HPP

#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#include "../fox_fifo/fox_protocol.hpp"

// Fox_Jobs turns the synchronous, single-command "fox" engine into an
// asynchronous, pollable job model so a remote dashboard can submit a command,
// get an id back immediately, and stream output + a final exit code.
//
// The engine itself stays serial: recovery has one action thread and mutates a
// lot of global state (PartitionManager, DataManager), so concurrent commands
// are unsafe. Fox_Jobs is therefore a *serialized queue*, not a parallel
// executor. One job runs at a time, gated by the same active-command/tw_busy
// guard the FIFO path uses.
//
// Threading:
//   - Submit()/Get*()/List*() are called from the HTTP server thread.
//   - BeginNext()/NotifyExit()/FinishActive() are called from the GUI thread
//     (Fox_Job_Dispatcher + the foxcmd action), which owns dispatch.
// All state is protected by an internal mutex.
class Fox_Jobs
{
public:
	enum State { PENDING, RUNNING, DONE };

	struct Snapshot {
		int id;
		State state;
		int exit_code;
		Fox_Rpc_Request request;
		size_t output_size;
		time_t created;
		time_t finished;
	};

	// ---- HTTP-thread side -------------------------------------------------

	// Queue a command. Returns the new job id (>0), or 0 if request is invalid.
	static int Submit(const Fox_Rpc_Request& request);

	// Copy the state of one job. Returns false if the id is unknown.
	static bool Get(int id, Snapshot& out);

	// Incremental output read. Copies bytes of job 'id' starting at 'offset'
	// into 'out'. On return, 'offset' is advanced to the new read position.
	// Returns false if the id is unknown.
	static bool ReadOutput(int id, size_t& offset, std::string& out);

	// Like ReadOutput(), but snapshots the job state under the same lock so the
	// caller cannot observe output from a job that is pruned before Get().
	static bool ReadOutputAndSnapshot(int id, size_t& offset, std::string& out,
	                                  Snapshot& snap);

	// JSON array describing every known job (newest first), no output bodies.
	static std::string ListJson();

	// ---- GUI-thread side --------------------------------------------------

	// True if a job is waiting to be dispatched and none is currently running.
	static bool HasPending();

	// Promote the next PENDING job to RUNNING and hand back what the GUI needs
	// to dispatch it: the request to run and a FILE* that receives the NDJSON
	// event stream. Returns false if nothing is pending or a job is already
	// running. The returned FILE* is owned by the caller and must be fclose()d
	// when the command finishes (closing it does not destroy the job).
	static bool BeginNext(FILE** out_file, Fox_Rpc_Request& out_request);

	// Record the exit code for the running job (no-op if none). Called from the
	// foxcmd action before the output FILE is closed.
	static void NotifyExit(int code);

	// Mark the running job DONE (no-op if none). Called after the foxcmd output
	// FILE has been closed/flushed.
	static void FinishActive();

	static int Active();  // id of the RUNNING job, or 0

private:
};

#endif // _FOX_JOBS_HPP
