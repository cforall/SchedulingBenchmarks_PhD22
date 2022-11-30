package main

import (
	"bufio"
	"flag"
	"fmt"
	"log"
	"os"
	"runtime"
	"runtime/pprof"
	"sync/atomic"
	"time"
)

var clock_mode bool
var threads_left int64
var stop int32
var duration float64
var stop_count uint64
var nprocs int
var nthreads int

func fflush(f *bufio.Writer) {
	defer f.Flush()
	f.Write([]byte("\r"))
}

func wait(start time.Time, is_tty bool) {
	f := bufio.NewWriter(os.Stdout)
	tdur := time.Duration(duration)
	for true {
		time.Sleep(100 * time.Millisecond)
		end := time.Now()
		delta := end.Sub(start)
		if is_tty {
			fmt.Printf(" %.1f",delta.Seconds())
			fflush(f)
		}
		if clock_mode && delta >= (tdur * time.Second) {
			break
		} else if !clock_mode && atomic.LoadInt64(&threads_left) == 0 {
			break
		}
	}
}

func bench_init() func() {
	nprocsOpt := flag.Int("p", 1, "The number of processors")
	nthreadsOpt := flag.Int("t", 1, "The number of threads")
	durationOpt := flag.Float64("d", 0, "Duration of the experiment in seconds")
	stopOpt := flag.Uint64("i", 0, "Duration of the experiment in iterations")
	cpuprofile := flag.String("cpuprofile", "", "write cpu profile to file")

	flag.Parse()

	nprocs = *nprocsOpt
	nthreads = *nthreadsOpt
	duration = *durationOpt
	stop_count = *stopOpt

	if duration > 0 && stop_count > 0 {
		panic(fmt.Sprintf("--duration and --iterations cannot be used together\n"))
	} else if duration > 0 {
		clock_mode = true
		stop_count = 0xFFFFFFFFFFFFFFFF
		fmt.Printf("Running for %f seconds\n", duration)
	} else if stop_count > 0 {
		clock_mode = false
		fmt.Printf("Running for %d iterations\n", stop_count)
	} else {
		duration = 5
		clock_mode = true
		fmt.Printf("Running for %f seconds (default)\n", duration)
	}

	runtime.GOMAXPROCS(nprocs)

	if (*cpuprofile) != "" {
		f, err := os.Create(*cpuprofile)
		if err != nil {
		    log.Fatal(err)
		}
		pprof.StartCPUProfile(f)
	}

	return func() {
		if (*cpuprofile) != "" {
			pprof.StopCPUProfile()
		}
	}
}