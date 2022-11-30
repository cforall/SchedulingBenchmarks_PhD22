package main

import (
	"context"
	"flag"
	"fmt"
	"math/rand"
	"os"
	"syscall"
	"sync/atomic"
	"time"
	"unsafe"
	"golang.org/x/sync/semaphore"
	"golang.org/x/text/language"
	"golang.org/x/text/message"
)

// ==================================================
type MyData struct {
	_p1 [16]uint64 // padding
	ttid int
	id int
	data [] uint64
	_p2 [16]uint64 // padding
}

func NewData(id int, size uint64) (*MyData) {
	var data [] uint64
	data = make([]uint64, size)
	for i := uint64(0); i < size; i++ {
		data[i] = 0
	}
	return &MyData{[16]uint64{0}, syscall.Gettid(), id, data,[16]uint64{0}}
}

func (this * MyData) moved( ttid int ) (uint64) {
	if this.ttid == ttid {
		return 0
	}
	this.ttid = ttid
	return 1
}

func (this * MyData) access( idx uint64 ) {
	this.data[idx % uint64(len(this.data))] += 1
}

// ==================================================
type MyCtx struct {
	_p1 [16]uint64 // padding
	s * semaphore.Weighted
	d unsafe.Pointer
	c context.Context
	ttid int
	id int
	_p2 [16]uint64 // padding
}

func NewCtx( data * MyData, id int ) (MyCtx) {
	r := MyCtx{[16]uint64{0},semaphore.NewWeighted(1), unsafe.Pointer(data), context.Background(), syscall.Gettid(), id,[16]uint64{0}}
	r.s.Acquire(context.Background(), 1)
	return r
}

func (this * MyCtx) moved( ttid int ) (uint64) {
	if this.ttid == ttid {
		return 0
	}
	this.ttid = ttid
	return 1
}

// ==================================================
// Atomic object where a single thread can wait
// May exchanges data
type Spot struct {
	_p1 [16]uint64 // padding
	ptr uintptr // atomic variable use fo MES
	id int      // id for debugging
	_p2 [16]uint64 // padding
}

// Main handshake of the code
// Single seat, first thread arriving waits
// Next threads unblocks current one and blocks in its place
// if share == true, exchange data in the process
func (this * Spot) put( ctx * MyCtx, data * MyData, share bool) (* MyData, bool) {
	new := uintptr(unsafe.Pointer(ctx))
	// old_d := ctx.d

	// Attempt to CAS our context into the seat
	var raw uintptr
	for true {
		raw = this.ptr
		if raw == uintptr(1) { // Seat is closed, return
			return nil, true
		}
		if atomic.CompareAndSwapUintptr(&this.ptr, raw, new) {
			break // We got the seat
		}
	}

	// If we aren't the fist in, wake someone
	if raw != uintptr(0) {
		var val *MyCtx
		val = (*MyCtx)(unsafe.Pointer(raw))

		// If we are sharing, give them our data
		if share {
			// fmt.Printf("[%d] - %d update %d: %p -> %p\n", this.id, ctx.id, val.id, val.d, data)
			atomic.StorePointer(&val.d, unsafe.Pointer(data))
		}

		// Wake them up
		// fmt.Printf("[%d] - %d release %d\n", this.id, ctx.id, val.id)
		val.s.Release(1)
	}

	// fmt.Printf("[%d] - %d enter\n", this.id, ctx.id)

	// Block once on the seat
	ctx.s.Acquire(ctx.c, 1)

	// Someone woke us up, get the new data
	ret := (* MyData)(atomic.LoadPointer(&ctx.d))
	// fmt.Printf("[%d] - %d leave: %p -> %p\n", this.id, ctx.id, ret, old_d)

	return ret, false
}

// Shutdown the spot
// Wake current thread and mark seat as closed
func (this * Spot) release() {
	val := (*MyCtx)(unsafe.Pointer(atomic.SwapUintptr(&this.ptr, uintptr(1))))
	if val == nil {
		return
	}

	// Someone was there, release them
	val.s.Release(1)
}

// ==================================================
// Struct for result, Go doesn't support passing tuple in channels
type Result struct {
	count uint64
	gmigs uint64
	dmigs uint64
}

func NewResult() (Result) {
	return Result{0, 0, 0}
}

// ==================================================
// Random number generator, Go's native one is to slow and global
func __xorshift64( state * uint64 ) (uint64) {
	x := *state
	x ^= x << 13
	x ^= x >> 7
	x ^= x << 17
	*state = x
	return x
}

// ==================================================
// Do some work by accessing 'cnt' cells in the array
func work(data * MyData, cnt uint64, state * uint64) {
	for i := uint64(0); i < cnt; i++ {
		data.access(__xorshift64(state))
	}
}

// Main body of the threads
func local(result chan Result, start chan struct{}, size uint64, cnt uint64, channels [] Spot, share bool, id int) {
	// Initialize some data
    	state := rand.Uint64()    // RNG state
	data := NewData(id, size) // Starting piece of data
	ctx := NewCtx(data, id)   // Goroutine local context

	// Prepare results
	r := NewResult()

	// Wait for start
	<- start

	// Main loop
	for true {
		// Touch our current data, write to invalidate remote cache lines
		work(data, cnt, &state)

		// Wait on a random spot
		i := __xorshift64(&state) % uint64(len(channels))
		var closed bool
		data, closed = channels[i].put(&ctx, data, share)

		// Check if the experiment is over
		if closed { break }                                       // yes, spot was closed
		if  clock_mode && atomic.LoadInt32(&stop) == 1 { break }  // yes, time's up
		if !clock_mode && r.count >= stop_count { break }         // yes, iterations reached

		// Check everything is consistent
		if uint64(len(data.data)) != size { panic("Data has weird size") }

		// write down progress and check migrations
		ttid := syscall.Gettid()
		r.count += 1
		r.gmigs += ctx .moved(ttid)
		r.dmigs += data.moved(ttid)
	}

	// Mark goroutine as done
	atomic.AddInt64(&threads_left, -1);

	// return result
	result <- r
}

// ==================================================
// Program main
func main() {
	// Benchmark specific command line arguments
	nspotsOpt    := flag.Int   ("n", 0    , "Number of spots where threads sleep (nthreads - nspots are active at the same time)")
	work_sizeOpt := flag.Uint64("w", 2    , "Size of the array for each threads, in words (64bit)")
	countOpt     := flag.Uint64("c", 2    , "Number of words to touch when working (random pick, cells can be picked more than once)")
	shareOpt     := flag.Bool  ("s", false, "Pass the work data to the next thread when blocking")

	// General benchmark initialization and deinitialization
	defer bench_init()()

	// Eval command line arguments
	nspots:= *nspotsOpt
	size  := *work_sizeOpt
	cnt   := *countOpt
	share := *shareOpt

	if nspots == 0 { nspots = nthreads - nprocs; }

	// Check params
	if ! (nthreads > nprocs) {
		fmt.Fprintf(os.Stderr, "Must have more threads than procs\n")
		os.Exit(1)
	}

	// Make global data
	barrierStart := make(chan struct{})         // Barrier used at the start
	threads_left = int64(nthreads - nspots)                // Counter for active threads (not 'nthreads' because at all times 'nthreads - nprocs' are blocked)
	result  := make(chan Result)                // Channel for results
	channels := make([]Spot, nspots) // Number of spots
	for i := range channels {
		channels[i] = Spot{[16]uint64{0},uintptr(0), i,[16]uint64{0}}     // init spots
	}

	// start the goroutines
	for i := 0; i < nthreads; i++ {
		go local(result, barrierStart, size, cnt, channels, share, i)
	}
	fmt.Printf("Starting\n");

	atomic.StoreInt32(&stop, 0)
	start := time.Now()
	close(barrierStart) // release barrier

	wait(start, true);  // general benchmark wait

	atomic.StoreInt32(&stop, 1)
	end := time.Now()
	delta := end.Sub(start)

	fmt.Printf("\nDone\n")

	// release all the blocked threads
	for i := range channels {
		channels[i].release()
	}

	// Join and accumulate results
	results := NewResult()
	for i := 0; i < nthreads; i++ {
		r := <- result
		results.count += r.count
		results.gmigs += r.gmigs
		results.dmigs += r.dmigs
	}

	// Print with nice 's, i.e. 1'000'000 instead of 1000000
	p := message.NewPrinter(language.English)
	p.Printf("Duration (ms)          : %d\n", delta.Milliseconds());
	p.Printf("Number of processors   : %d\n", nprocs);
	p.Printf("Number of threads      : %d\n", nthreads);
	p.Printf("Work size (64bit words): %d\n", size);
	if share {
		p.Printf("Data sharing           : On\n");
	} else {
		p.Printf("Data sharing           : Off\n");
	}
	p.Printf("Total Operations(ops)  : %15d\n", results.count)
	p.Printf("Total G Migrations     : %15d\n", results.gmigs)
	p.Printf("Total D Migrations     : %15d\n", results.dmigs)
	p.Printf("Ops per second         : %18.2f\n", float64(results.count) / delta.Seconds())
	p.Printf("ns per ops             : %18.2f\n", float64(delta.Nanoseconds()) / float64(results.count))
	p.Printf("Ops per threads        : %15d\n", results.count / uint64(nthreads))
	p.Printf("Ops per procs          : %15d\n", results.count / uint64(nprocs))
	p.Printf("Ops/sec/procs          : %18.2f\n", (float64(results.count) / float64(nprocs)) / delta.Seconds())
	p.Printf("ns per ops/procs       : %18.2f\n", float64(delta.Nanoseconds()) / (float64(results.count) / float64(nprocs)))
}