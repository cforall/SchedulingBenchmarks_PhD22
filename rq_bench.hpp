#include <cassert>
#include <climits>
#include <cstdint>
#include <cstdio>

#include <time.h>										// timespec
#include <sys/time.h>									// timeval

typedef __uint128_t __lehmer64_state_t;
static inline uint64_t __lehmer64( __lehmer64_state_t & state ) {
	state *= 0xda942042e4dd58b5;
	return state >> 64;
}

enum { TIMEGRAN = 1000000000LL };					// nanosecond granularity, except for timeval


volatile bool stop = false;
bool clock_mode;
double duration = -1;
unsigned long long stop_count = 0;
unsigned nprocs = 1;
unsigned nthreads = 1;

volatile unsigned long long threads_left;

#define BENCH_OPT \
	{'d', "duration",  "Duration of the experiments in seconds", duration }, \
	{'i', "iterations",  "Number of iterations of the experiments", stop_count }, \
	{'t', "nthreads",  "Number of threads to use", nthreads }, \
	{'p', "nprocs",    "Number of processors to use", nprocs }

#define BENCH_OPT_PARSE(name) \
	{ \
		int opt_cnt = sizeof(opt) / sizeof(option_t); \
		char **left; \
		parse_args( argc, argv, opt, opt_cnt, "[OPTIONS]...\n" name, &left ); \
		if(duration > 0 && stop_count > 0) { \
			fprintf(stderr, "--duration and --iterations cannot be used together\n"); \
			print_args_usage(argc, argv, opt, opt_cnt, "[OPTIONS]...\n" name, true); \
		} else if(duration > 0) { \
			clock_mode = true; \
			stop_count = 0xFFFFFFFFFFFFFFFF; \
			printf("Running for %lf seconds\n", duration); \
		} else if(stop_count > 0) { \
			clock_mode = false; \
			printf("Running for %llu iterations\n", stop_count); \
		} else { \
			duration = 5; clock_mode = true;\
			printf("Running for %lf seconds\n", duration); \
		} \
	}

uint64_t timeHiRes() {
	timespec curr;
	clock_gettime( CLOCK_REALTIME, &curr );
	return (int64_t)curr.tv_sec * TIMEGRAN + curr.tv_nsec;
}

uint64_t to_miliseconds( uint64_t durtn ) { return durtn / (TIMEGRAN / 1000LL); }
double to_fseconds(uint64_t durtn ) { return durtn / (double)TIMEGRAN; }
uint64_t from_fseconds(double sec) { return sec * TIMEGRAN; }

template<typename Sleeper>
void wait(const uint64_t & start, bool is_tty) {
	for(;;) {
		Sleeper::usleep(100000);
		uint64_t end = timeHiRes();
		uint64_t delta = end - start;
		if(is_tty) {
			printf(" %.1f\r", to_fseconds(delta));
			fflush(stdout);
		}
		if( clock_mode && delta >= from_fseconds(duration) ) {
			break;
		}
		else if( !clock_mode && threads_left == 0 ) {
			break;
		}
	}
}

class Fibre;
int fibre_park();
int fibre_unpark( Fibre * );
Fibre * fibre_self();

class __attribute__((aligned(128))) bench_sem {
	Fibre * volatile ptr = nullptr;
public:
	inline bool wait() {
		static Fibre * const ready  = reinterpret_cast<Fibre *>(1ull);
		for(;;) {
			Fibre * expected = this->ptr;
			if(expected == ready) {
				if(__atomic_compare_exchange_n(&this->ptr, &expected, nullptr, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
					return false;
				}
			}
			else {
				/* paranoid */ assert( expected == nullptr );
				if(__atomic_compare_exchange_n(&this->ptr, &expected, fibre_self(), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
					fibre_park();
					return true;
				}
			}

		}
	}

	inline bool post() {
		static Fibre * const ready  = reinterpret_cast<Fibre *>(1ull);
		for(;;) {
			Fibre * expected = this->ptr;
			if(expected == ready) return false;
			if(expected == nullptr) {
				if(__atomic_compare_exchange_n(&this->ptr, &expected, ready, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
					return false;
				}
			}
			else {
				if(__atomic_compare_exchange_n(&this->ptr, &expected, nullptr, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
					fibre_unpark( expected );
					return true;
				}
			}
		}
	}
};

// ==========================================================================================
#include <cstdlib>
#include <cstring>

#include <algorithm>

//-----------------------------------------------------------------------------
// Typed argument parsing
bool parse_yesno(const char * arg, bool & value ) {
	if(strcmp(arg, "yes") == 0) {
		value = true;
		return true;
	}

	if(strcmp(arg, "Y") == 0) {
		value = true;
		return true;
	}

	if(strcmp(arg, "y") == 0) {
		value = true;
		return true;
	}

	if(strcmp(arg, "no") == 0) {
		value = false;
		return true;
	}

	if(strcmp(arg, "N") == 0) {
		value = false;
		return true;
	}

	if(strcmp(arg, "n") == 0) {
		value = false;
		return true;
	}

	return false;
}

bool parse_truefalse(const char * arg, bool & value) {
	if(strcmp(arg, "true") == 0) {
		value = true;
		return true;
	}

	if(strcmp(arg, "false") == 0) {
		value = false;
		return true;
	}

	return false;
}

bool parse_settrue (const char *, bool & value ) {
	value = true;
	return true;
}

bool parse_setfalse(const char *, bool & value )  {
	value = false;
	return true;
}

bool parse(const char * arg, const char * & value ) {
	value = arg;
	return true;
}

bool parse(const char * arg, int & value) {
	char * end;
	int r = strtoll(arg, &end, 10);
	if(*end != '\0') return false;

	value = r;
	return true;
}

bool parse(const char * arg, unsigned & value) {
	char * end;
	unsigned long long int r = strtoull(arg, &end, 10);
	if(*end != '\0') return false;
	if(r > UINT_MAX) return false;

	value = r;
	return true;
}

bool parse(const char * arg, unsigned long & value) {
	char * end;
	unsigned long long int r = strtoull(arg, &end, 10);
	if(*end != '\0') return false;
	if(r > ULONG_MAX) return false;

	value = r;
	return true;
}

bool parse(const char * arg, unsigned long long & value) {
        char * end;
        unsigned long long int r = strtoull(arg, &end, 10);
        if(*end != '\0') return false;
        if(r > ULLONG_MAX) return false;

        value = r;
        return true;
}

bool parse(const char * arg, double & value) {
	char * end;
	double r = strtod(arg, &end);
	if(*end != '\0') return false;

	value = r;
	return true;
}

//-----------------------------------------------------------------------------
struct option_t {
      char short_name;
      const char * long_name;
      const char * help;
      void * variable;
      bool (*parse_fun)(const char *, void * );

	template<typename T>
	inline option_t( char short_name, const char * long_name, const char * help, T & variable ) {
		this->short_name = short_name;
		this->long_name  = long_name;
		this->help       = help;
		this->variable   = reinterpret_cast<void*>(&variable);
		#pragma GCC diagnostic push
		#pragma GCC diagnostic ignored "-Wcast-function-type"
				this->parse_fun  = reinterpret_cast<bool (*)(const char *, void * )>(static_cast<bool (*)(const char *, T & )>(parse));
		#pragma GCC diagnostic pop
	}

	template<typename T>
	inline option_t( char short_name, const char * long_name, const char * help, T & variable, bool (*parse)(const char *, T & )) {
		this->short_name = short_name;
		this->long_name  = long_name;
		this->help       = help;
		this->variable   = reinterpret_cast<void*>(&variable);
		#pragma GCC diagnostic push
		#pragma GCC diagnostic ignored "-Wcast-function-type"
			this->parse_fun  = reinterpret_cast<bool (*)(const char *, void * )>(parse);
		#pragma GCC diagnostic pop
	}
};

extern option_t last_option;


//-----------------------------------------------------------------------------
#include <cstdint>
#include <climits>
#include <errno.h>
#include <unistd.h>
extern "C" {
	#include <getopt.h>
	#include <sys/ioctl.h>

	extern FILE * stderr;
	extern FILE * stdout;

	extern int fileno(FILE *stream);

	extern int fprintf ( FILE * stream, const char * format, ... );

	extern          long long int strtoll (const char* str, char** endptr, int base);
	extern unsigned long long int strtoull(const char* str, char** endptr, int base);
	extern                 double strtod  (const char* str, char** endptr);
}

static void usage(char * cmd, option_t options[], size_t opt_count, const char * usage, FILE * out)  __attribute__ ((noreturn));

//-----------------------------------------------------------------------------
// getopt_long wrapping
void parse_args(
	int argc,
	char * argv[],
	option_t options[],
	size_t opt_count,
	const char * usage_msg,
	char ** * left
) {
	struct option optarr[opt_count + 2];
	{
		int idx = 0;
		for(size_t i = 0; i < opt_count; i++) {
			if(options[i].long_name) {
				optarr[idx].name = options[i].long_name;
				optarr[idx].flag = nullptr;
				optarr[idx].val  = options[i].short_name;
				if(    ((intptr_t)options[i].parse_fun) == ((intptr_t)parse_settrue)
				    || ((intptr_t)options[i].parse_fun) == ((intptr_t)parse_setfalse) ) {
					optarr[idx].has_arg = no_argument;
				} else {
					optarr[idx].has_arg = required_argument;
				}
				idx++;
			}
		}
		optarr[idx+0].name = "help";
		optarr[idx+0].has_arg = no_argument;
		optarr[idx+0].flag = 0;
		optarr[idx+0].val = 'h';
		optarr[idx+1].name = 0;
		optarr[idx+1].has_arg = no_argument;
		optarr[idx+1].flag = 0;
		optarr[idx+1].val = 0;
	}

	char optstring[opt_count * 3];
	for(auto & o : optstring) {
		o = '\0';
	}
	{
		int idx = 0;
		for(size_t i = 0; i < opt_count; i++) {
			optstring[idx] = options[i].short_name;
			idx++;
			if(    ((intptr_t)options[i].parse_fun) != ((intptr_t)parse_settrue)
			    && ((intptr_t)options[i].parse_fun) != ((intptr_t)parse_setfalse) ) {
				optstring[idx] = ':';
				idx++;
			}
		}
		optstring[idx+0] = 'h';
		optstring[idx+1] = '\0';
	}

	FILE * out = stderr;
	for(;;) {
		int idx = 0;
		int opt = getopt_long(argc, argv, optstring, optarr, &idx);
		switch(opt) {
			case -1:
				if(left != nullptr) *left = argv + optind;
				return;
			case 'h':
				out = stdout;
				[[fallthrough]];
			case '?':
				usage(argv[0], options, opt_count, usage_msg, out);
			default:
				for(size_t i = 0; i < opt_count; i++) {
					if(opt == options[i].short_name) {
						const char * arg = optarg ? optarg : "";
						if( arg[0] == '=' ) { arg++; }
						bool success = options[i].parse_fun( arg, options[i].variable );
						if(success) goto NEXT_ARG;

						fprintf(out, "Argument '%s' for option %c could not be parsed\n\n", arg, (char)opt);
						usage(argv[0], options, opt_count, usage_msg, out);
					}
				}
				std::abort();
		}
		NEXT_ARG:;
	}
}

//-----------------------------------------------------------------------------
// Print usage
static void printopt(FILE * out, int width, int max, char sn, const char * ln, const char * help) {
	int hwidth = max - (11 + width);
	if(hwidth <= 0) hwidth = max;

	fprintf(out, "  -%c, --%-*s   %.*s\n", sn, width, ln, hwidth, help);
	for(;;) {
		help += std::min(strlen(help), (unsigned long)hwidth);
		if('\0' == *help) break;
		fprintf(out, "%*s%.*s\n", width + 11, "", hwidth, help);
	}
}

__attribute__((noreturn)) void print_args_usage(int , char * argv[], option_t options[], size_t opt_count, const char * usage_msg, bool error) {
	usage(argv[0], options, opt_count, usage_msg, error ? stderr : stdout);
}

static __attribute__((noreturn)) void usage(char * cmd, option_t options[], size_t opt_count, const char * help, FILE * out) {
	int width = 0;
	{
		for(size_t i = 0; i < opt_count; i++) {
			if(options[i].long_name) {
				int w = strlen(options[i].long_name);
				if(w > width) width = w;
			}
		}
	}

	int max_width = 1000000;
	int outfd = fileno(out);
	if(isatty(outfd)) {
		struct winsize size;
		int ret = ioctl(outfd, TIOCGWINSZ, &size);
		if(ret < 0) abort(); // "ioctl error: (%d) %s\n", (int)errno, strerror(errno)
		max_width = size.ws_col;
	}

	fprintf(out, "Usage:\n  %s %s\n", cmd, help);

	for(size_t i = 0; i < opt_count; i++) {
		printopt(out, width, max_width, options[i].short_name, options[i].long_name, options[i].help);
	}
	fprintf(out, "  -%c, --%-*s   %s\n", 'h', width, "help", "print this help message");
	exit(out == stdout ? 0 : 1);
}

