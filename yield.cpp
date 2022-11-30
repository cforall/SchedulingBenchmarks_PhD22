#include "rq_bench.hpp"
#include <libfibre/fibre.h>

volatile bool run = false;
volatile unsigned long long global_counter;


void fibre_main() {
	fibre_park();
	unsigned long long count = 0;
	for(;;) {
		Fibre::yield();
		count++;
		if( clock_mode && stop) break;
		if(!clock_mode && count >= stop_count) break;
	}

	__atomic_fetch_add(&global_counter, count, __ATOMIC_SEQ_CST);
	__atomic_fetch_add(&threads_left, -1, __ATOMIC_SEQ_CST);
}

int main(int argc, char * argv[]) {
	option_t opt[] = {
		BENCH_OPT
	};
	BENCH_OPT_PARSE("libfibre yield benchmark");

	{
		printf("Running %d threads on %d processors for %lf seconds\n", nthreads, nprocs, duration);

		FibreInit(1, nprocs);
		uint64_t start, end;
		{
			threads_left = nthreads;
			Fibre ** threads = new Fibre *[nthreads]();
			for(unsigned i = 0; i < nthreads; i++) {
				threads[i] = new Fibre();
				threads[i]->run(fibre_main);
			}
			printf("Starting\n");
			bool is_tty = isatty(STDOUT_FILENO);
			start = timeHiRes();

			for(unsigned i = 0; i < nthreads; i++ ) {
				fibre_unpark( threads[i] );
			}
			wait<Fibre>(start, is_tty);

			stop = true;
			end = timeHiRes();
			for(unsigned i = 0; i < nthreads; i++ ) {
				fibre_join( threads[i], nullptr );
			}
			delete[] threads;
		}

		printf("Duration (ms)        : %'ld\n", to_miliseconds(end - start));
		printf("Number of processors : %'d\n", nprocs);
		printf("Number of threads    : %'d\n", nthreads);
		printf("Total Operations(ops): %'15llu\n", global_counter);
		printf("Ops per second       : %'18.2lf\n", ((double)global_counter) / to_fseconds(end - start));
		printf("ns per ops           : %'18.2lf\n", ((double)(end - start)) / global_counter);
		printf("Ops per threads      : %'15llu\n", global_counter / nthreads);
		printf("Ops per procs        : %'15llu\n", global_counter / nprocs);
		printf("Ops/sec/procs        : %'18.2lf\n", (((double)global_counter) / nprocs) / to_fseconds(end - start));
		printf("ns per ops/procs     : %'18.2lf\n", ((double)(end - start)) / (global_counter / nprocs));
		fflush(stdout);
	}
}
