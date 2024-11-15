
/*******************************************************************************
 * Copyright IBM Corp. and others 1991
 *
 * This program and the accompanying materials are made available under
 * the terms of the Eclipse Public License 2.0 which accompanies this
 * distribution and is available at https://www.eclipse.org/legal/epl-2.0/
 * or the Apache License, Version 2.0 which accompanies this distribution and
 * is available at https://www.apache.org/licenses/LICENSE-2.0.
 *
 * This Source Code may also be made available under the following
 * Secondary Licenses when the conditions for such availability set
 * forth in the Eclipse Public License, v. 2.0 are satisfied: GNU
 * General Public License, version 2 with the GNU Classpath
 * Exception [1] and GNU General Public License, version 2 with the
 * OpenJDK Assembly Exception [2].
 *
 * [1] https://www.gnu.org/software/classpath/license.html
 * [2] https://openjdk.org/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0-only WITH Classpath-exception-2.0 OR GPL-2.0-only WITH OpenJDK-assembly-exception-1.0
 *******************************************************************************/

/**
 * @file
 * @ingroup GC_Modron_Tarok
 */

#if !defined(GLOBALMARKDELEGATE_HPP_)
#define GLOBALMARKDELEGATE_HPP_

#include "j9.h"
#include "j9cfg.h"
#include "EnvironmentVLHGC.hpp"
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
# include <cstdio> // for FILE, fopen, fclose, printf
# include <memory> // for unique_ptr


class MM_CycleState;
class MM_ParallelDispatcher;
class MM_InterRegionRememberedSet;
class MM_GCExtensions;
class MM_GlobalMarkingScheme;
class MM_MarkMap;
class MM_WorkPacketsVLHGC;

struct fileCloserDuplicate {
	void operator()(FILE *file) const {
		if (file) {
			printf("My log: fileCloser\n");
			fclose(file);
		}
	}
};


class MM_GlobalMarkDelegate : public MM_BaseNonVirtual
{
	/* Data Members */
public:
protected:
private:
	J9JavaVM *_javaVM;  /**< Current JVM */
	MM_GCExtensions *_extensions;  /**< Global variable container for GC */
	MM_GlobalMarkingScheme *_markingScheme;  /**< Bit map marking implementation used to perform liveness tracing */
	MM_ParallelDispatcher *_dispatcher;  /**< Dispatcher used for tasks */

	FILE *_dump_fout;
	std::unique_ptr<FILE, fileCloserDuplicate> _dump_ptr;

	/* Member Functions */
public:

	void initializeDumpFile() {
		char fileName[40];
		std::ignore = tmpnam(fileName);
		_dump_ptr.reset(fopen(fileName, "w"));
		if (!_dump_ptr) {
			printf("My log: initializeDumpFile failed to open file.");
			return;
		}
		_dump_fout = _dump_ptr.get();

		printf("My log: initializeDumpFile in GlobalMarkDelegate with name='%s'\n", fileName);
	}

	void fetchPageBits(void *vaddr, uintptr_t numPages) {
		fprintf(_dump_fout, "fetchPageBits starts: ");
		fprintf(_dump_fout, "vaddr: %p, numPages: %ld. [page idx, present, flags]\n", vaddr, numPages);

		// TODO: fetch page size from JVM
		// const uint64_t PAGE_SIZE = 4096;
		long PAGE_SIZE = sysconf(_SC_PAGE_SIZE);

		const uint64_t PFN_FLAG = ((1ULL << 55) - 1); // PFN  (0-54 bits)
		const uint64_t PRESENT_FLAG = 1ULL << 63; // present bit (63rd bit)
		

		// page flags refer to linux/include/uapi/linux/kernel-page-flags.h

		int pagemap_fd = open("/proc/self/pagemap", O_RDONLY);
		int kpageflags_fd = open("/proc/kpageflags", O_RDONLY);
		if (pagemap_fd < 0) {
			fprintf(_dump_fout, "Failed to open /proc/self/pagemap\n");
			return;
		}
		if (kpageflags_fd < 0) {
			fprintf(_dump_fout, "Failed to open /proc/kpageflags\n");
			return;
		}

		uintptr_t base_addr = reinterpret_cast<uintptr_t>(vaddr);

		for (uintptr_t i = 0; i < numPages; ++i)
		{
			uintptr_t page_addr = base_addr + (i * PAGE_SIZE);
			uintptr_t page_index = page_addr / PAGE_SIZE;
			off_t pagemap_offset = page_index * sizeof(uint64_t);

			uint64_t pagemap_entry;
			ssize_t bytes_read = pread(pagemap_fd, &pagemap_entry, sizeof(pagemap_entry), pagemap_offset);
			if (bytes_read != sizeof(pagemap_entry)) {
				fprintf(_dump_fout, "Failed to read pagemap entry for page addr 0x%lx (offset: 0x%lx): %s\n", 
						page_addr, pagemap_offset, strerror(errno));
				continue;
			}

			uint64_t pfn = pagemap_entry & PFN_FLAG; 
			bool present = (pagemap_entry & PRESENT_FLAG) != 0;

			fprintf(_dump_fout, "%lu, %d", i, present ? 1 : 0);

			if (present && pfn != 0) {
				off_t kpageflags_offset = pfn * sizeof(uint64_t);
				uint64_t kpageflags;

				bytes_read = pread(kpageflags_fd, &kpageflags, sizeof(kpageflags), kpageflags_offset);
				if (bytes_read != sizeof(kpageflags)) {
					fprintf(_dump_fout, "Failed to read kpageflags entry for pfn 0x%lx (offset: 0x%lx): %s\n", 
							pfn, kpageflags_offset, strerror(errno));
					continue;
				}

				fprintf(_dump_fout, ", 0x%08x\n", static_cast<uint32_t>(kpageflags & 0xFFFFFFFF));
			} else {
				fprintf(_dump_fout, "\n");
			}

		}

		close(pagemap_fd);
		close(kpageflags_fd);
		fprintf(_dump_fout, "fetchPageBits done\n\n");
	}


	/**
	 * Initialize any resources required.
	 */
	bool initialize(MM_EnvironmentVLHGC *env);

	/**
	 * Clean up any resources in use.
	 */
	void tearDown(MM_EnvironmentVLHGC *env);

	/**
	 * Adjust internal structures to reflect the change in heap size.
	 *
	 * @param size The amount of memory added to the heap
	 * @param lowAddress The base address of the memory added to the heap
	 * @param highAddress The top address (non-inclusive) of the memory added to the heap
	 * @return true if operation completes with success
	 */
	bool heapAddRange(MM_EnvironmentVLHGC *env, MM_MemorySubSpace *subspace, UDATA size, void *lowAddress, void *highAddress);

	/**
	 * Adjust internal structures to reflect the change in heap size.
	 *
	 * @param size The amount of memory added to the heap
	 * @param lowAddress The base address of the memory added to the heap
	 * @param highAddress The top address (non-inclusive) of the memory added to the heap
	 * @param lowValidAddress The first valid address previous to the lowest in the heap range being removed
	 * @param highValidAddress The first valid address following the highest in the heap range being removed
	 * @return true if operation completes with success
	 */
	bool heapRemoveRange(MM_EnvironmentVLHGC *env, MM_MemorySubSpace *subspace, UDATA size, void *lowAddress, void *highAddress, void *lowValidAddress, void *highValidAddress);

	/**
	 * Set initial state for incremental Mark
	 * @param env[in] The main GC thread
	 */
	void performMarkSetInitialState(MM_EnvironmentVLHGC *env);

	/**
	 * Execute complete Mark operation.
	 * Try to use results of Incremental Mark steps done before if any
	 * 
	 * @param env[in] The thread which called driveGlobalCollectMainThread
	 */
	void performMarkForGlobalGC(MM_EnvironmentVLHGC *env);

	/**
	 * Execute one step of incremental Mark.
	 * 
	 * @param env[in] The thread which called driveGlobalCollectMainThread
	 * @param markIncrementEndTime[in] end of time interval scheduled for step (as observed by time_hires_clock in ticks)
	 * @return true if last step of incremental Mark has been executed
	 */
	bool performMarkIncremental(MM_EnvironmentVLHGC *env, U_64 markIncrementEndTime);

	/**
	 * Execute one step of concurrent GMP Mark.
	 * 
	 * @param env[in] The main GC thread for the concurrent operation
	 * @param totalBytesToScan[in] The number of bytes which must be scanned by the concurrent task before it stops attempting to do more work
	 * @param forceExit[in] A "kill switch" which, when set to true, implies that the concurrent operation should terminate (volatile as it is written by another thread)
	 * @return The number of bytes scanned by the concurrent mark task
	 */
	UDATA performMarkConcurrent(MM_EnvironmentVLHGC *env, UDATA totalBytesToScan, volatile bool *forceExit);

	/**
	 * Executes mark initialization only and advances the mark state machine to the root step.  The caller must ensure that the mark state
	 * machine is set to the initial state, prior to calling this method (otherwise, it will assert).
	 * 
	 * @param env[in] The main GC thread
	 */
	void performMarkInit(MM_EnvironmentVLHGC *env);

	/**
	 * Perform cleanup after mark.
	 * This includes reporting object delete events, unloading classes and starting finalization
	 *
	 * @param env[in] The main thread
	 */
	void postMarkCleanup(MM_EnvironmentVLHGC *env);

	MM_GlobalMarkDelegate (MM_EnvironmentBase *env) :
		MM_BaseNonVirtual()
		, _javaVM(NULL)
		, _extensions(NULL)
		, _markingScheme(NULL)
		, _dispatcher(NULL)
	{
		_typeId = __FUNCTION__;
	}

protected:
	
private:

	/**
	 *	Mark operation - Complete Mark operation.
	 */
	void markAll(MM_EnvironmentVLHGC *env);

	/**
	 *	Mark operation - Initialization.
	 *	@param timeThreshold[in] The scheduled end time by which the operation is expected to end, expressed in ticks
	 *	@return True if the operation reached timeThreshold before it completed the operation 
	 */
	bool markInit(MM_EnvironmentVLHGC *env, U_64 timeThreshold);

	/**
	 *	Mark operation - Marking Roots.
	 */
	void markRoots(MM_EnvironmentVLHGC *env);

	/**
	 *	Mark operation - Scan work packets.
	 *	@param timeThreshold[in] The scheduled end time by which the operation is expected to end, expressed in ticks
	 *	@return True if the operation reached timeThreshold before it completed the operation 
	 */
	bool markScan(MM_EnvironmentVLHGC *env, U_64 timeThreshold);

	/**
	 *	Mark operation - Use any remaining increment time to scrub the card table.
	 *	@param timeThreshold[in] The scheduled end time by which the operation is expected to end, expressed in ticks
	 *	@return True if the operation reached timeThreshold before it completed the operation 
	 */
	bool markScrubCardTable(MM_EnvironmentVLHGC *env, U_64 timeThreshold);

	/**
	 *	Mark operation - Complete.
	 */
	void markComplete(MM_EnvironmentVLHGC *env);

};

#endif /* GLOBALMARKDELEGATE_HPP_ */
