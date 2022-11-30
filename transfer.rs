#[cfg(debug_assertions)]
use std::io::{self, Write};

use std::process;
use std::option;
use std::hint;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::time::{Instant,Duration};

use tokio::runtime::Builder;
use tokio::sync;
use tokio::task;

use rand::Rng;

use clap::{Arg, App};
use num_format::{Locale, ToFormattedString};

#[path = "../bench.rs"]
mod bench;

#[cfg(debug_assertions)]
macro_rules! debug {
	($x:expr) => {
		println!( $x );
		io::stdout().flush().unwrap();
	};
	($x:expr, $($more:expr),+) => {
		println!( $x, $($more), * );
		io::stdout().flush().unwrap();
	};
}

#[cfg(not(debug_assertions))]
macro_rules! debug {
    ($x:expr   ) => { () };
    ($x:expr, $($more:expr),+) => { () };
}

fn parse_yes_no(opt: option::Option<&str>, default: bool) -> bool {
	match opt {
		Some(val) => {
			match val {
				"yes" => true,
				"Y" => true,
				"y" => true,
				"no"  => false,
				"N"  => false,
				"n"  => false,
				"maybe" | "I don't know" | "Can you repeat the question?" => {
					eprintln!("Lines for 'Malcolm in the Middle' are not acceptable values of parameter 'exhaust'");
					std::process::exit(1);
				},
				_ => {
					eprintln!("parameter 'exhaust' must have value 'yes' or 'no', was {}", val);
					std::process::exit(1);
				},
			}
		},
		_ => {
			default
		},
	}
}

struct LeaderInfo {
	id: AtomicUsize,
	idx: AtomicUsize,
	estop: AtomicBool,
	seed: u128,
}

impl LeaderInfo {
	pub fn new(nthreads: usize) -> LeaderInfo {
		let this = LeaderInfo{
			id: AtomicUsize::new(nthreads),
			idx: AtomicUsize::new(0),
			estop: AtomicBool::new(false),
			seed: process::id() as u128
		};

		let mut rng = rand::thread_rng();

		for _ in 0..rng.gen_range(0..10) {
			this.next( nthreads );
		}

		this
	}

	pub fn next(&self, len: usize) {
		let n = bench::_lehmer64( unsafe {
			let r1 = &self.seed as *const u128;
			let r2 = r1 as *mut u128;
			&mut *r2
		} ) as usize;
		self.id.store( n % len , Ordering::SeqCst );
	}
}

struct MyThread {
	id: usize,
	idx: AtomicUsize,
	sem: sync::Semaphore,
}

fn waitgroup(leader: &LeaderInfo, idx: usize, threads: &Vec<Arc<MyThread>>, main_sem: &sync::Semaphore) {
	let start = Instant::now();
	'outer: for t in threads {
		debug!( "Waiting for :{} ({})", t.id, t.idx.load(Ordering::Relaxed) );
		while t.idx.load(Ordering::Relaxed) != idx {
			hint::spin_loop();
			if start.elapsed() > Duration::from_secs(5) {
				eprintln!("Programs has been blocked for more than 5 secs");
				leader.estop.store(true, Ordering::Relaxed);
				main_sem.add_permits(1);
				break 'outer;
			}
		}
	}
	debug!( "Waiting done" );
}

fn wakegroup(exhaust: bool, me: usize, threads: &Vec<Arc<MyThread>>) {
	if !exhaust { return; }

	for i in 0..threads.len() {
		if i != me {
			debug!( "Leader waking {}", i);
			threads[i].sem.add_permits(1);
		}
	}
}

fn lead(exhaust: bool, leader: &LeaderInfo, this: & MyThread, threads: &Vec<Arc<MyThread>>, main_sem: &sync::Semaphore, exp: &bench::BenchData) {
	let nidx = leader.idx.load(Ordering::Relaxed) + 1;
	this.idx.store(nidx, Ordering::Relaxed);
	leader.idx.store(nidx, Ordering::Relaxed);

	if nidx as u64 > exp.stop_count || leader.estop.load(Ordering::Relaxed) {
		debug!( "Leader {} done", this.id);
		main_sem.add_permits(1);
		return;
	}

	debug!( "====================\nLeader no {} : {}", nidx, this.id);

	waitgroup(leader, nidx, threads, main_sem);

	leader.next( threads.len() );

	wakegroup(exhaust, this.id, threads);

	debug!( "Leader no {} : {} done\n====================", nidx, this.id);
}

async fn wait(exhaust: bool, leader: &LeaderInfo, this: &MyThread, rechecks: &mut usize) {
	task::yield_now().await;

	if leader.idx.load(Ordering::Relaxed) == this.idx.load(Ordering::Relaxed) {
		debug!("Waiting {} recheck", this.id);
		*rechecks += 1;
		return;
	}

	debug!("Waiting {}", this.id);

	debug_assert!( (leader.idx.load(Ordering::Relaxed) - 1) == this.idx.load(Ordering::Relaxed) );
	this.idx.fetch_add(1, Ordering::SeqCst);
	if exhaust {
		debug!("Waiting {} sem", this.id);
		this.sem.acquire().await.forget();
	}
	else {
		debug!("Waiting {} yield", this.id);
		task::yield_now().await;
	}

	debug!("Waiting {} done", this.id);
}

async fn transfer_main( me: usize, leader: Arc<LeaderInfo>, threads: Arc<Vec<Arc<MyThread>>>, exhaust: bool, start: Arc<sync::Barrier>, main_sem: Arc<sync::Semaphore>, exp: Arc<bench::BenchData>) -> usize{
	assert!( me == threads[me].id );

	debug!("Ready {}: {:p}", me, &threads[me].sem as *const sync::Semaphore);

	start.wait().await;

	debug!( "Start {}", me );

	let mut rechecks: usize = 0;

	loop {
		if leader.id.load(Ordering::Relaxed) == me {
			lead( exhaust, &leader, &threads[me], &threads, &main_sem, &exp );
			task::yield_now().await;
		}
		else {
			wait( exhaust, &leader, &threads[me], &mut rechecks ).await;
		}
		if leader.idx.load(Ordering::Relaxed) as u64 > exp.stop_count || leader.estop.load(Ordering::Relaxed) { break; }
	}

	rechecks
}

fn main() {
	let options = App::new("Transfer Tokio")
		.args(&bench::args())
		.arg(Arg::with_name("exhaust")  .short("e").long("exhaust")  .takes_value(true).default_value("no").help("Whether or not threads that have seen the new epoch should park instead of yielding."))
		.get_matches();

	let exhaust  = parse_yes_no( options.value_of("exhaust"), false );
	let nthreads = options.value_of("nthreads").unwrap().parse::<usize>().unwrap();
	let nprocs   = options.value_of("nprocs").unwrap().parse::<usize>().unwrap();


	let exp = Arc::new(bench::BenchData::new(options, nthreads, Some(100)));
	if exp.clock_mode {
		eprintln!("Programs does not support fixed duration mode");
		std::process::exit(1);
	}

	println!("Running {} threads on {} processors, doing {} iterations, {} exhaustion", nthreads, nprocs, exp.stop_count, if exhaust { "with" } else { "without" });

	let s = (1000000 as u64).to_formatted_string(&Locale::en);
	assert_eq!(&s, "1,000,000");

	let main_sem = Arc::new(sync::Semaphore::new(0));
	let leader = Arc::new(LeaderInfo::new(nthreads));
	let barr = Arc::new(sync::Barrier::new(nthreads + 1));
	let thddata : Arc<Vec<Arc<MyThread>>> = Arc::new(
		(0..nthreads).map(|i| {
			Arc::new(MyThread{
				id: i,
				idx: AtomicUsize::new(0),
				sem: sync::Semaphore::new(0),
			})
		}).collect()
	);

	let mut rechecks: usize = 0;
	let mut duration : std::time::Duration = std::time::Duration::from_secs(0);

	let runtime = Builder::new_multi_thread()
		.worker_threads(nprocs)
		.enable_all()
		.build()
		.unwrap();

	runtime.block_on(async {
		let threads: Vec<_> = (0..nthreads).map(|i| {
			tokio::spawn(transfer_main(i, leader.clone(), thddata.clone(), exhaust, barr.clone(), main_sem.clone(), exp.clone()))
		}).collect();
		println!("Starting");

		let start = Instant::now();

		barr.wait().await;
		debug!("Unlocked all");


		main_sem.acquire().await.forget();

		duration = start.elapsed();

		println!("\nDone");


		for i in 0..nthreads {
			thddata[i].sem.add_permits(1);
		}

		for t in threads {
			rechecks += t.await.unwrap();
		}
	});

	println!("Duration (ms)           : {}", (duration.as_millis()).to_formatted_string(&Locale::en));
	println!("Number of processors    : {}", (nprocs).to_formatted_string(&Locale::en));
	println!("Number of threads       : {}", (nthreads).to_formatted_string(&Locale::en));
	println!("Total Operations(ops)   : {:>15}", (leader.idx.load(Ordering::Relaxed) - 1).to_formatted_string(&Locale::en));
	println!("Threads parking on wait : {}", if exhaust { "yes" } else { "no" });
	println!("Rechecking              : {}", rechecks );
	println!("ns per transfer         : {}", ((duration.as_nanos() as f64) / leader.idx.load(Ordering::Relaxed) as f64));

}