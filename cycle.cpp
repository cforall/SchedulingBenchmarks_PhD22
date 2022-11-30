
#include "rq_bench.hpp"
#include <libfibre/fibre.h>

struct Partner {
	unsigned long long count  = 0;
	unsigned long long blocks = 0;
	bench_sem self;
	bench_sem * next;
};

void partner_main( Partner * self ) {
	self->count = 0;
	for(;;) {
		self->blocks += self->self.wait();
		self->next->post();
		self->count ++;
		if( clock_mode && stop) break;
		if(!clock_mode && self->count >= stop_count) break;
	}

	__atomic_fetch_add(&threads_left, -1, __ATOMIC_SEQ_CST);
}

int main(int argc, char * argv[]) {
	unsigned ring_size = 2;
	option_t opt[] = {
		BENCH_OPT,
		{ 'r', "ringsize", "Number of threads in a cycle", ring_size }
	};
	BENCH_OPT_PARSE("libfibre cycle benchmark");

	{
		unsigned long long global_counter = 0;
		unsigned long long global_blocks  = 0;
		unsigned tthreads = nthreads * ring_size;
		uint64_t start, end;
		FibreInit(1, nprocs);
		{
			threads_left = tthreads;
			Fibre ** threads = new Fibre *[tthreads]();
			Partner* thddata = new Partner[tthreads]();
			for(unsigned i = 0; i < tthreads; i++) {
				unsigned pi = (i + nthreads) % tthreads;
				thddata[i].next = &thddata[pi].self;
			}
			for(unsigned i = 0; i < tthreads; i++) {
				threads[i] = new Fibre();
				threads[i]->run( partner_main, &thddata[i] );
			}
			printf("Starting\n");

			bool is_tty = isatty(STDOUT_FILENO);
			start = timeHiRes();

			for(unsigned i = 0; i < nthreads; i++) {
				thddata[i].self.post();
			}
			wait<Fibre>(start, is_tty);

			stop = true;
			end = timeHiRes();
			printf("\nDone\n");

			for(unsigned i = 0; i < tthreads; i++) {
				thddata[i].self.post();
				fibre_join( threads[i], nullptr );
				global_counter += thddata[i].count;
				global_blocks  += thddata[i].blocks;
			}

			delete[](threads);
			delete[](thddata);
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
