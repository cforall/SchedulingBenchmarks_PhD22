#include "rq_bench.hpp"

#include <pthread.h>
#include <semaphore.h>
#include <sched.h>
#include <unistd.h>

struct Pthread {
	static int usleep(useconds_t usec) {
		return ::usleep(usec);
	}
};

struct Partner {
	unsigned long long count  = 0;
	unsigned long long blocks = 0;
	sem_t self;
	sem_t * next;
};

void partner_main( Partner * self ) {
	self->count = 0;
	for(;;) {
		sem_wait(&self->self);
		sem_post(self->next);
		self->count ++;
		if( clock_mode && stop) break;
		if(!clock_mode && self->count >= stop_count) break;

		int sval;
		sem_getvalue(&self->self, &sval);
		if(sval > 1) std::abort();
		if(sval < 0) std::abort();
	}

	__atomic_fetch_add(&threads_left, -1, __ATOMIC_SEQ_CST);
}

int main(int argc, char * argv[]) {
	unsigned ring_size = 2;
	option_t opt[] = {
		BENCH_OPT,
		{ 'r', "ringsize", "Number of threads in a cycle", ring_size }
	};
	BENCH_OPT_PARSE("cforall cycle benchmark");

	{
		unsigned long long global_counter = 0;
		unsigned long long global_blocks  = 0;
		unsigned tthreads = nthreads * ring_size;
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
			threads_left = tthreads;
			pthread_t threads[tthreads];
			Partner thddata[tthreads];
			for(int i = 0; i < tthreads; i++) {
				int ret = sem_init( &thddata[i].self, false, 0 );
				if(ret != 0) std::abort();

				unsigned pi = (i + nthreads) % tthreads;
				thddata[i].next = &thddata[pi].self;
			}
			for(int i = 0; i < tthreads; i++) {
				int ret = pthread_create( &threads[i], nullptr, reinterpret_cast<void * (*)(void *)>(partner_main), &thddata[i] );
				if(ret != 0) std::abort();
			}
			printf("Starting\n");

			bool is_tty = isatty(STDOUT_FILENO);
			start = timeHiRes();

			for(int i = 0; i < nthreads; i++) {
				sem_post(&thddata[i].self);
			}
			wait<Pthread>(start, is_tty);

			stop = true;
			end = timeHiRes();
			printf("\nDone\n");

			for(int i = 0; i < tthreads; i++) {
				sem_post(&thddata[i].self);
				int ret = pthread_join( threads[i], nullptr );
				if(ret != 0) std::abort();
				global_counter += thddata[i].count;
				global_blocks  += thddata[i].blocks;
			}

			for(int i = 0; i < tthreads; i++) {
				int ret = sem_destroy( &thddata[i].self );
				if(ret != 0) std::abort();
			}
		}

		printf("Duration (ms)        : %'ld\n", to_miliseconds(end - start));
		printf("Number of processors : %'d\n", nprocs);
		printf("Number of threads    : %'d\n", tthreads);
		printf("Cycle size (# thrds) : %'d\n", ring_size);
		printf("Total Operations(ops): %'15llu\n", global_counter);
		printf("Total blocks         : %'15llu\n", global_blocks);
		printf("Ops per second       : %'18.2lf\n", ((double)global_counter) / to_fseconds(end - start));
		printf("ns per ops           : %'18.2lf\n", ((double)(end - start)) / global_counter);
		printf("Ops per threads      : %'15llu\n", global_counter / tthreads);
		printf("Ops per procs        : %'15llu\n", global_counter / nprocs);
		printf("Ops/sec/procs        : %'18.2lf\n", (((double)global_counter) / nprocs) / to_fseconds(end - start));
		printf("ns per ops/procs     : %'18.2lf\n", ((double)(end - start)) / (global_counter / nprocs));
		fflush(stdout);
	}

	return 0;
}
