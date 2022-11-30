#include "rq_bench.hpp"

#include <pthread.h>
#include <semaphore.h>
#include <sched.h>
#include <unistd.h>

#include <iostream>

struct Result {
	uint64_t count = 0;
	uint64_t dmigs = 0;
	uint64_t gmigs = 0;
};

struct Pthread {
	static int usleep(useconds_t usec) {
		return ::usleep(usec);
	}
};

// ==================================================
struct __attribute__((aligned(128))) MyData {
	uint64_t _p1[16];  // padding
	uint64_t * data;
	size_t len;
	int ttid;
	size_t id;
	uint64_t _p2[16];  // padding

	MyData(size_t id, size_t size)
		: data( (uintptr_t *)aligned_alloc(128, size * sizeof(uint64_t)) )
		, len( size )
		, ttid( sched_getcpu() )
		, id( id )
	{
		for(size_t i = 0; i < this->len; i++) {
			this->data[i] = 0;
		}
	}

	uint64_t moved(int ttid) {
		if(this->ttid == ttid) {
			return 0;
		}
		this->ttid = ttid;
		return 1;
	}

	__attribute__((noinline)) void access(size_t idx) {
		size_t l = this->len;
		this->data[idx % l] += 1;
	}
};

// ==================================================
struct __attribute__((aligned(128))) MyCtx {
	struct MyData * volatile data;

	struct {
		struct MySpot ** ptr;
		size_t len;
	} spots;

	sem_t sem;

	Result result;

	bool share;
	size_t cnt;
	int ttid;
	size_t id;

	MyCtx(MyData * d, MySpot ** spots, size_t len, size_t cnt, bool share, size_t id)
		: data( d )
		, spots{ .ptr = spots, .len = len }
		, share( share )
		, cnt( cnt )
		, ttid( sched_getcpu() )
		, id( id )
	{
		int ret = sem_init( &sem, false, 0 );
		if(ret != 0) std::abort();
	}

	~MyCtx() {
		int ret = sem_destroy( &sem );
		if(ret != 0) std::abort();
	}

	uint64_t moved(int ttid) {
		if(this->ttid == ttid) {
			return 0;
		}
		this->ttid = ttid;
		return 1;
	}
};

// ==================================================
// Atomic object where a single thread can wait
// May exchanges data
struct __attribute__((aligned(128))) MySpot {
	MyCtx * volatile ptr;
	size_t id;
	uint64_t _p1[16];  // padding

	MySpot(size_t id) : ptr( nullptr ), id( id ) {}


	static inline MyCtx * one() {
		return reinterpret_cast<MyCtx *>(1);
	}

	// Main handshake of the code
	// Single seat, first thread arriving waits
	// Next threads unblocks current one and blocks in its place
	// if share == true, exchange data in the process
	bool put( MyCtx & ctx, MyData * data, bool share) {
		// Attempt to CAS our context into the seat
		for(;;) {
			MyCtx * expected = this->ptr;
			if (expected == one()) { // Seat is closed, return
				return true;
			}

			if (__atomic_compare_exchange_n(&this->ptr, &expected, &ctx, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
				if(expected) {
					if(share) {
						expected->data = data;
					}
					sem_post(&expected->sem);
				}
				break; // We got the seat
			}
		}

		// Block once on the seat
		sem_wait(&ctx.sem);

		// Someone woke us up, get the new data
		return false;
	}

	// Shutdown the spot
	// Wake current thread and mark seat as closed
	void release() {
		struct MyCtx * val = __atomic_exchange_n(&this->ptr, one(), __ATOMIC_SEQ_CST);
		if (!val) {
			return;
		}

		// Someone was there, release them
		sem_post(&val->sem);
	}
};

// ==================================================
// Random number generator, Go's native one is to slow and global
uint64_t __xorshift64( uint64_t & state ) {
	uint64_t x = state;
	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	return state = x;
}

// ==================================================
// Do some work by accessing 'cnt' cells in the array
__attribute__((noinline)) void work(MyData & data, size_t cnt, uint64_t & state) {
	for (size_t i = 0; i < cnt; i++) {
		data.access(__xorshift64(state));
	}
}

void thread_main( MyCtx & ctx ) {
	uint64_t state = ctx.id;

	// Wait for start
	sem_wait(&ctx.sem);

	// Main loop
	for(;;) {
		// Touch our current data, write to invalidate remote cache lines
		work( *ctx.data, ctx.cnt, state );

		// Wait on a random spot
		uint64_t idx = __xorshift64(state) % ctx.spots.len;
		bool closed = ctx.spots.ptr[idx]->put(ctx, ctx.data, ctx.share);

		// Check if the experiment is over
		if (closed) break;
		if ( clock_mode && stop) break;
		if (!clock_mode && ctx.result.count >= stop_count) break;

		// Check everything is consistent
		assert( ctx.data );

		// write down progress and check migrations
		int ttid = sched_getcpu();
		ctx.result.count += 1;
		ctx.result.gmigs += ctx.moved(ttid);
		ctx.result.dmigs += ctx.data->moved(ttid);
	}

	__atomic_fetch_add(&threads_left, -1, __ATOMIC_SEQ_CST);
}

// ==================================================
int main(int argc, char * argv[]) {
	unsigned wsize = 2;
	unsigned wcnt  = 2;
	unsigned nspots = 0;
	bool share = false;
	option_t opt[] = {
		BENCH_OPT,
		{ 'n', "nspots", "Number of spots where threads sleep (nthreads - nspots are active at the same time)", nspots},
		{ 'w', "worksize", "Size of the array for each threads, in words (64bit)", wsize},
		{ 'c', "workcnt" , "Number of words to touch when working (random pick, cells can be picked more than once)", wcnt },
		{ 's', "share"   , "Pass the work data to the next thread when blocking", share, parse_truefalse }
	};
	BENCH_OPT_PARSE("libfibre cycle benchmark");

	std::cout.imbue(std::locale(""));
	setlocale(LC_ALL, "");

	unsigned long long global_count = 0;
	unsigned long long global_gmigs = 0;
	unsigned long long global_dmigs = 0;

	if( nspots == 0 ) { nspots = nthreads - nprocs; }

	uint64_t start, end;
	{
		cpu_set_t cpuset;
		int ret = pthread_getaffinity_np( pthread_self(), sizeof(cpuset), &cpuset );
		if(ret != 0) std::abort();

		unsigned cnt = CPU_COUNT_S(sizeof(cpuset), &cpuset);
		if(cnt > nprocs) {
			unsigned extras = cnt - nprocs;
			for(int i = 0; i < CPU_SETSIZE && extras > 0; i++) {
				if(CPU_ISSET_S(i, sizeof(cpuset), &cpuset)) {
					CPU_CLR_S(i, sizeof(cpuset), &cpuset);
					extras--;
				}
			}

			ret = pthread_setaffinity_np( pthread_self(), sizeof(cpuset), &cpuset );
			if(ret != 0) std::abort();
		}
	}

	{
		MyData * data_arrays[nthreads];
		for(size_t i = 0; i < nthreads; i++) {
			data_arrays[i] = new MyData( i, wsize );
		}

		MySpot * spots[nspots];
		for(unsigned i = 0; i < nspots; i++) {
			spots[i] = new MySpot{ i };
		}

		threads_left = nthreads - nspots;
		pthread_t threads[nthreads];
		MyCtx * thddata[nthreads];
		{
			for(size_t i = 0; i < nthreads; i++) {
				thddata[i] = new MyCtx(
					data_arrays[i],
					spots,
					nspots,
					wcnt,
					share,
					i
				);
				int ret = pthread_create( &threads[i], nullptr, reinterpret_cast<void * (*)(void *)>(thread_main), thddata[i] );
				if(ret != 0) std::abort();
			}

			bool is_tty = isatty(STDOUT_FILENO);
			start = timeHiRes();

			for(size_t i = 0; i < nthreads; i++) {
				sem_post(&thddata[i]->sem);
			}
			wait<Pthread>(start, is_tty);

			stop = true;
			end = timeHiRes();
			printf("\nDone\n");

			for(size_t i = 0; i < nthreads; i++) {
				sem_post(&thddata[i]->sem);
				int ret = pthread_join( threads[i], nullptr );
				if(ret != 0) std::abort();
				global_count += thddata[i]->result.count;
				global_gmigs += thddata[i]->result.gmigs;
				global_dmigs += thddata[i]->result.dmigs;
			}
		}

		for(size_t i = 0; i < nthreads; i++) {
			delete( data_arrays[i] );
		}

		for(size_t i = 0; i < nspots; i++) {
			delete( spots[i] );
		}
	}

	printf("Duration (ms)          : %'ld\n", to_miliseconds(end - start));
	printf("Number of processors   : %'d\n", nprocs);
	printf("Number of threads      : %'d\n", nthreads);
	printf("Number of spots        : %'d\n", nspots);
	printf("Work size (64bit words): %'15u\n", wsize);
	printf("Total Operations(ops)  : %'15llu\n", global_count);
	printf("Total G Migrations     : %'15llu\n", global_gmigs);
	printf("Total D Migrations     : %'15llu\n", global_dmigs);
	printf("Ops per second         : %'18.2lf\n", ((double)global_count) / to_fseconds(end - start));
	printf("ns per ops             : %'18.2lf\n", ((double)(end - start)) / global_count);
	printf("Ops per threads        : %'15llu\n", global_count / nthreads);
	printf("Ops per procs          : %'15llu\n", global_count / nprocs);
	printf("Ops/sec/procs          : %'18.2lf\n", (((double)global_count) / nprocs) / to_fseconds(end - start));
	printf("ns per ops/procs       : %'18.2lf\n", ((double)(end - start)) / (global_count / nprocs));
	fflush(stdout);
}
