use std::ptr;
use std::sync::Arc;
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::Instant;
use std::thread::{self, ThreadId};

use tokio::runtime::Builder;
use tokio::sync;

use clap::{App, Arg};
use num_format::{Locale, ToFormattedString};
use rand::Rng;

#[path = "../bench.rs"]
mod bench;

// ==================================================
struct MyData {
	_p1: [u64; 16],
	data: Vec<u64>,
	ttid: ThreadId,
	_id: usize,
	_p2: [u64; 16],
}

impl MyData {
	fn new(id: usize, size: usize) -> MyData {
		MyData {
			_p1: [0; 16],
			data: vec![0; size],
			ttid: thread::current().id(),
			_id: id,
			_p2: [0; 16],
		}
	}

	fn moved(&mut self, ttid: ThreadId) -> u64 {
		if self.ttid == ttid {
			return 0;
		}
		self.ttid = ttid;
		return 1;
	}

	fn access(&mut self, idx: usize) {
		let l = self.data.len();
		self.data[idx % l] += 1;
	}
}

struct MyDataPtr {
	ptr: *mut MyData,
}

unsafe impl std::marker::Send for MyDataPtr{}

// ==================================================
struct MyCtx {
	_p1: [u64; 16],
	s: sync::Semaphore,
	d: MyDataPtr,
	ttid: ThreadId,
	_id: usize,
	_p2: [u64; 16],
}

impl MyCtx {
	fn new(d: *mut MyData, id: usize) -> MyCtx {
		MyCtx {
			_p1: [0; 16],
			s: sync::Semaphore::new(0),
			d: MyDataPtr{ ptr: d },
			ttid: thread::current().id(),
			_id: id,
			_p2: [0; 16],
		}
	}

	fn moved(&mut self, ttid: ThreadId) -> u64 {
		if self.ttid == ttid {
			return 0;
		}
		self.ttid = ttid;
		return 1;
	}
}
// ==================================================
// Atomic object where a single thread can wait
// May exchanges data
struct MySpot {
	_p1: [u64; 16],
	ptr: AtomicU64,
	_id: usize,
	_p2: [u64; 16],
}

impl MySpot {
	fn new(id: usize) -> MySpot {
		let r = MySpot{
			_p1: [0; 16],
			ptr: AtomicU64::new(0),
			_id: id,
			_p2: [0; 16],
		};
		r
	}

	fn one() -> u64 {
		1
	}

	// Main handshake of the code
	// Single seat, first thread arriving waits
	// Next threads unblocks current one and blocks in its place
	// if share == true, exchange data in the process
	async fn put( &self, ctx: &mut MyCtx, data: MyDataPtr, share: bool) -> (*mut MyData, bool) {
		{
			// Attempt to CAS our context into the seat
			let raw = {
				loop {
					let expected = self.ptr.load(Ordering::Relaxed) as u64;
					if expected == MySpot::one() { // Seat is closed, return
						let r: *const MyData = ptr::null();
						return (r as *mut MyData, true);
					}
					let got = self.ptr.compare_exchange_weak(expected, ctx as *mut MyCtx as u64, Ordering::SeqCst, Ordering::SeqCst);
					if got == Ok(expected) {
						break expected;// We got the seat
					}
				}
			};

			// If we aren't the fist in, wake someone
			if raw != 0 {
				let val: &mut MyCtx = unsafe{ &mut *(raw as *mut MyCtx) };
				// If we are sharing, give them our data
				if share {
					val.d.ptr = data.ptr;
				}

				// Wake them up
				val.s.add_permits(1);
			}
		}

		// Block once on the seat
		ctx.s.acquire().await.forget();

		// Someone woke us up, get the new data
		let ret = ctx.d.ptr;
		return (ret, false);
	}

	// Shutdown the spot
	// Wake current thread and mark seat as closed
	fn release(&self) {
		let val = self.ptr.swap(MySpot::one(), Ordering::SeqCst);
		if val == 0 {
			return
		}

		// Someone was there, release them
		unsafe{ &mut *(val as *mut MyCtx) }.s.add_permits(1)
	}
}

// ==================================================
// Struct for result, Go doesn't support passing tuple in channels
struct Result {
	count: u64,
	gmigs: u64,
	dmigs: u64,
}

impl Result {
	fn new() -> Result {
		Result{ count: 0, gmigs: 0, dmigs: 0}
	}

	fn add(&mut self, o: Result) {
		self.count += o.count;
		self.gmigs += o.gmigs;
		self.dmigs += o.dmigs;
	}
}

// ==================================================
// Random number generator, Go's native one is to slow and global
fn __xorshift64( state: &mut u64 ) -> usize {
	let mut x = *state;
	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	*state = x;
	x as usize
}

// ==================================================
// Do some work by accessing 'cnt' cells in the array
fn work(data: &mut MyData, cnt: u64, state : &mut u64) {
	for _ in 0..cnt {
		data.access(__xorshift64(state))
	}
}

async fn local(start: Arc<sync::Barrier>, idata: MyDataPtr, spots : Arc<Vec<MySpot>>, cnt: u64, share: bool, id: usize, exp: Arc<bench::BenchData>) -> Result{
	let mut state = rand::thread_rng().gen::<u64>();
	let mut data = idata;
	let mut ctx = MyCtx::new(data.ptr, id);
	let _size = unsafe{ &mut *data.ptr }.data.len();

	// Prepare results
	let mut r = Result::new();

	// Wait for start
	start.wait().await;

	// Main loop
	loop {
		// Touch our current data, write to invalidate remote cache lines
		work(unsafe{ &mut *data.ptr }, cnt, &mut state);

		// Wait on a random spot
		let i = (__xorshift64(&mut state) as usize) % spots.len();
		let closed = {
			let (d, c) = spots[i].put(&mut ctx, data, share).await;
			data = MyDataPtr{ ptr: d };
			c
		};

		// Check if the experiment is over
		if closed { break }                                                   // yes, spot was closed
		if  exp.clock_mode && exp.stop.load(Ordering::Relaxed) { break }  // yes, time's up
		if !exp.clock_mode && r.count >= exp.stop_count { break }         // yes, iterations reached

		assert_ne!(data.ptr as *const MyData, ptr::null());

		let d = unsafe{ &mut *data.ptr };

		// Check everything is consistent
		debug_assert_eq!(d.data.len(), _size);

		// write down progress and check migrations
		let ttid = thread::current().id();
		r.count += 1;
		r.gmigs += ctx .moved(ttid);
		r.dmigs += d.moved(ttid);
	}

	exp.threads_left.fetch_sub(1, Ordering::SeqCst);
	r
}


// ==================================================
fn main() {
	let options = App::new("Locality Tokio")
		.args(&bench::args())
		.arg(Arg::with_name("nspots").short("n").long("nspots")  .takes_value(true).default_value("0").help("Number of spots where threads sleep (nthreads - nspots are active at the same time)"))
		.arg(Arg::with_name("size")  .short("w").long("worksize").takes_value(true).default_value("2").help("Size of the array for each threads, in words (64bit)"))
		.arg(Arg::with_name("work")  .short("c").long("workcnt") .takes_value(true).default_value("2").help("Number of words to touch when working (random pick, cells can be picked more than once)"))
		.arg(Arg::with_name("share") .short("s").long("share")   .takes_value(true).default_value("true").help("Pass the work data to the next thread when blocking"))
		.get_matches();

	let nthreads   = options.value_of("nthreads").unwrap().parse::<usize>().unwrap();
	let nprocs     = options.value_of("nprocs").unwrap().parse::<usize>().unwrap();
	let wsize      = options.value_of("size").unwrap().parse::<usize>().unwrap();
	let wcnt       = options.value_of("work").unwrap().parse::<u64>().unwrap();
	let share      = options.value_of("share").unwrap().parse::<bool>().unwrap();
	let nspots = {
		let val = options.value_of("nspots").unwrap().parse::<usize>().unwrap();
		if val != 0 {
			val
		} else {
			nthreads - nprocs
		}
	};

	// Check params
	if ! (nthreads > nprocs) {
		panic!("Must have more threads than procs");
	}

	let s = (1000000 as u64).to_formatted_string(&Locale::en);
	assert_eq!(&s, "1,000,000");

	let exp = Arc::new(bench::BenchData::new(options, nprocs, None));
	let mut results = Result::new();

	let mut elapsed : std::time::Duration = std::time::Duration::from_secs(0);

	let mut data_arrays : Vec<MyData> = (0..nthreads).map(|i| MyData::new(i, wsize)).rev().collect();
	let spots : Arc<Vec<MySpot>> = Arc::new((0..nspots).map(|i| MySpot::new(i)).rev().collect());
	let barr = Arc::new(sync::Barrier::new(nthreads + 1));

	let runtime = Builder::new_multi_thread()
		.worker_threads(nprocs)
		.enable_all()
		.build()
		.unwrap();

	runtime.block_on(async
		{
			let thrds: Vec<_> = (0..nthreads).map(|i| {
				debug_assert!(i < data_arrays.len());

				runtime.spawn(local(
					barr.clone(),
					MyDataPtr{ ptr: &mut data_arrays[i] },
					spots.clone(),
					wcnt,
					share,
					i,
					exp.clone(),
				))
			}).collect();


			println!("Starting");

			let start = Instant::now();
			barr.wait().await;

			elapsed = exp.wait(&start).await;

			println!("\nDone");

			// release all the blocked threads
			for s in &* spots {
				s.release();
			}

			println!("Threads released");

			// Join and accumulate results
			for t in thrds {
				results.add( t.await.unwrap() );
			}

			println!("Threads joined");
		}
	);

	println!("Duration (ms)          : {}", (elapsed.as_millis()).to_formatted_string(&Locale::en));
	println!("Number of processors   : {}", (nprocs).to_formatted_string(&Locale::en));
	println!("Number of threads      : {}", (nthreads).to_formatted_string(&Locale::en));
	println!("Work size (64bit words): {}", (wsize).to_formatted_string(&Locale::en));
	println!("Data sharing           : {}", if share { "On" } else { "Off" });
	println!("Total Operations(ops)  : {:>15}", (results.count).to_formatted_string(&Locale::en));
	println!("Total G Migrations     : {:>15}", (results.gmigs).to_formatted_string(&Locale::en));
	println!("Total D Migrations     : {:>15}", (results.dmigs).to_formatted_string(&Locale::en));
	println!("Ops per second         : {:>15}", (((results.count as f64) / elapsed.as_secs() as f64) as u64).to_formatted_string(&Locale::en));
	println!("ns per ops             : {:>15}", ((elapsed.as_nanos() as f64 / results.count as f64) as u64).to_formatted_string(&Locale::en));
	println!("Ops per threads        : {:>15}", (results.count / nthreads as u64).to_formatted_string(&Locale::en));
	println!("Ops per procs          : {:>15}", (results.count / nprocs as u64).to_formatted_string(&Locale::en));
	println!("Ops/sec/procs          : {:>15}", ((((results.count as f64) / nprocs as f64) / elapsed.as_secs() as f64) as u64).to_formatted_string(&Locale::en));
	println!("ns per ops/procs       : {:>15}", ((elapsed.as_nanos() as f64 / (results.count as f64 / nprocs as f64)) as u64).to_formatted_string(&Locale::en));
}