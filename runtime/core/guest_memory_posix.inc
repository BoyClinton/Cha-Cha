// GuestMemory on Linux (included by guest_memory.cpp inside namespace mh3u::core, in its anonymous namespace).
// The 4 GB block is an mmap(PROT_NONE) reservation; commit() makes pages readable and writable.
// Write tracking emulates Windows' write watch: pages in the watched ranges are made read-only; the first write to one
// faults, the SIGSEGV handler marks the page written and makes it writable again, and the scanner stamps it and makes
// it read-only again. A page stays marked for two passes, so a write that lands just after the handler returned (the
// faulting instruction itself) is still stamped. The kernel does not fault on its own writes (read() into a read-only
// page fails with EFAULT): file reads into guest memory go through a bounce buffer (fs/host_tree.cpp).

constexpr uint8_t kCommitted = 1, kTracked = 2;  // page state bits; kTracked: read-only, waiting for a write
std::unique_ptr<std::atomic<uint8_t>[]> g_page_state;
std::unique_ptr<std::atomic<uint8_t>[]> g_dirty;  // 2: written since the scanner last looked; 1: the pass before
bool g_report = false;
struct sigaction g_previous[2];

bool hit_watch(uint32_t addr, bool write) {
    std::lock_guard g(g_watch_m);
    for (size_t i = 0; i < g_watches.size(); ++i) {
        const Watch w = g_watches[i];
        if (addr < w.begin || addr >= w.end) continue;
        mprotect(g_base + w.begin, w.end - w.begin, PROT_READ | PROT_WRITE);
        g_watches.erase(g_watches.begin() + static_cast<std::ptrdiff_t>(i));
        std::fprintf(stderr, "watch: CPU %s guest %08X in watched range %08X-%08X (tag %08X)\n", write ? "write to" : "read from",
                     addr, w.begin, w.end, w.tag);
        return true;
    }
    return false;
}

void on_fault(int sig, siginfo_t* info, void* context) {
    const auto host = reinterpret_cast<uint64_t>(info->si_addr), base = reinterpret_cast<uint64_t>(g_base);
    const auto* uc = static_cast<const ucontext_t*>(context);
    const bool write = (uc->uc_mcontext.gregs[REG_ERR] & 2) != 0;
    if (g_base && host >= base && host < base + kSpace) {
        const auto addr = static_cast<uint32_t>(host - base);
        const size_t page = addr >> 12;
        const uint8_t state = g_page_state[page].load(std::memory_order_relaxed);
        if (state & kTracked) {  // a write to a tracked page
            g_dirty[page].store(2, std::memory_order_relaxed);
            g_page_state[page].fetch_and(static_cast<uint8_t>(~kTracked), std::memory_order_relaxed);
            mprotect(g_base + (page << 12), 0x1000, PROT_READ | PROT_WRITE);
            return;
        }
        if (hit_watch(addr, write)) return;
        if (write && (state & kCommitted)) {
            // read-only without kTracked: the scanner queued this page for protection, this handler unprotected it
            // for an earlier write, then the scanner's mprotect landed. Still a tracked write (the next pass sets
            // kTracked again from the dirty mark).
            g_dirty[page].store(2, std::memory_order_relaxed);
            mprotect(g_base + (page << 12), 0x1000, PROT_READ | PROT_WRITE);
            return;
        }
    }
    if (!g_report) {  // not ours: the previous handler (or the default action: re-raised when this returns)
        sigaction(sig, &g_previous[sig == SIGBUS ? 1 : 0], nullptr);
        return;
    }
    if (g_base && host >= base && host < base + kSpace)
        std::fprintf(stderr, "guest memory fault: %s guest address %08X (uncommitted)\n", write ? "write to" : "read from",
                     static_cast<uint32_t>(host - base));
    else
        std::fprintf(stderr, "host access violation at %p\n", info->si_addr);
    // where: module + offset of the faulting instruction (addr2line -e <module> <offset>)
    const auto pc = reinterpret_cast<void*>(uc->uc_mcontext.gregs[REG_RIP]);
    Dl_info dl{};
    if (dladdr(pc, &dl) && dl.dli_fname)
        std::fprintf(stderr, "  at %s+0x%llX (%s)\n", dl.dli_fname,
                     static_cast<unsigned long long>(reinterpret_cast<uint64_t>(pc) - reinterpret_cast<uint64_t>(dl.dli_fbase)),
                     dl.dli_sname ? dl.dli_sname : "?");
    std::fflush(stderr);
    _exit(3);
}

// [first, first + n) pages: one mprotect per run
void protect_pages(size_t first, size_t n, int prot) {
    if (n) mprotect(g_base + (first << 12), n << 12, prot);
}

}  // namespace

static GuestMemory* g_instance = nullptr;

GuestMemory::GuestMemory() {
    void* p = mmap(nullptr, kSpace, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) throw std::runtime_error("cannot reserve 4 GB of address space for guest memory");
    base_ = static_cast<uint8_t*>(p);
    g_base = base_;
    g_instance = this;
    page_epoch_ = std::make_unique<std::atomic<uint32_t>[]>(kSpace >> 12);
    g_page_state = std::make_unique<std::atomic<uint8_t>[]>(kSpace >> 12);
    g_dirty = std::make_unique<std::atomic<uint8_t>[]>(kSpace >> 12);
    struct sigaction sa{};
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &g_previous[0]);
    sigaction(SIGBUS, &sa, &g_previous[1]);
}

GuestMemory* GuestMemory::current() { return g_instance; }

void GuestMemory::set_watched(std::vector<Range> ranges) {
    {
        std::lock_guard g(watch_m_);
        watched_ = std::move(ranges);
    }
    if (!scanner_.joinable()) scanner_ = std::thread([this] { scan_loop(); });
}

// Each pass: pages written since the last one (or the one before) are stamped with epoch e and made read-only again;
// committed pages of the ranges not tracked yet are made read-only. Then e is published (release), as on Windows.
void GuestMemory::scan_loop() {
    pthread_setname_np(pthread_self(), "guest-writes");
    std::vector<Range> ranges;
    while (!stop_.load()) {
        {
            std::lock_guard g(watch_m_);
            ranges = watched_;
        }
        const uint32_t e = epoch_.load(std::memory_order_relaxed) + 1;
        uint64_t stamped = 0;
        for (const Range& r : ranges) {
            const size_t first = r.addr >> 12, last = (size_t{r.addr} + r.size + 0xFFF) >> 12;
            size_t run = 0, run_n = 0;  // pages to make read-only, as one run
            for (size_t pg = first; pg < last; ++pg) {
                const uint8_t st = g_page_state[pg].load(std::memory_order_relaxed);
                uint8_t d = g_dirty[pg].load(std::memory_order_relaxed);
                bool protect = false;
                if (d) {
                    g_page_state[pg].fetch_or(kTracked, std::memory_order_relaxed);
                    protect = true;
                    page_epoch_[pg].store(e, std::memory_order_relaxed);
                    ++stamped;
                    g_dirty[pg].compare_exchange_strong(d, static_cast<uint8_t>(d - 1), std::memory_order_relaxed);
                } else if ((st & kCommitted) && !(st & kTracked)) {
                    g_page_state[pg].fetch_or(kTracked, std::memory_order_relaxed);
                    protect = true;
                }
                if (protect && run_n && run + run_n == pg) {
                    ++run_n;
                } else if (protect) {
                    protect_pages(run, run_n, PROT_READ);
                    run = pg;
                    run_n = 1;
                }
            }
            protect_pages(run, run_n, PROT_READ);
        }
        epoch_.store(e, std::memory_order_release);
        scan_pages_.fetch_add(stamped, std::memory_order_relaxed);
        scans_.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
}

bool GuestMemory::written_since(uint32_t addr, uint32_t size, uint32_t since) const {
    if (!size) return false;
    const uint64_t end = std::min<uint64_t>(uint64_t{addr} + size, kSpace);
    for (uint64_t p = addr >> 12; p < (end + 0xFFF) >> 12; ++p)
        if (page_epoch_[p].load(std::memory_order_relaxed) > since) return true;
    return false;
}

GuestMemory::~GuestMemory() {
    stop_ = true;
    if (scanner_.joinable()) scanner_.join();
    if (base_) munmap(base_, kSpace);
    if (g_base == base_) g_base = nullptr;
}

bool GuestMemory::commit(uint32_t addr, uint32_t size) {
    const size_t first = addr >> 12, last = (size_t{addr} + size + 0xFFF) >> 12;
    if (mprotect(base_ + (first << 12), (last - first) << 12, PROT_READ | PROT_WRITE) != 0) return false;
    for (size_t p = first; p < last; ++p) g_page_state[p].store(kCommitted, std::memory_order_relaxed);
    return true;
}

bool GuestMemory::committed(uint32_t addr) const { return g_page_state[addr >> 12].load(std::memory_order_relaxed) & kCommitted; }

std::vector<uint32_t> GuestMemory::find(const uint8_t* pattern, size_t n, uint32_t from, uint32_t to, size_t max) const {
    std::vector<uint32_t> out;
    if (!n) return out;
    const std::boyer_moore_horspool_searcher search(pattern, pattern + n);
    const auto readable = [&](uint64_t page) {
        if (!(g_page_state[page].load(std::memory_order_relaxed) & kCommitted)) return false;
        std::lock_guard g(g_watch_m);  // diagnostic watches are inaccessible
        for (const Watch& w : g_watches)
            if ((page << 12) >= w.begin && (page << 12) < w.end) return false;
        return true;
    };
    for (uint64_t at = from >> 12; at < (uint64_t{to} + 0xFFF) >> 12 && out.size() < max;) {
        if (!readable(at)) {
            ++at;
            continue;
        }
        uint64_t end = at + 1;
        while (end < (uint64_t{to} + 0xFFF) >> 12 && readable(end)) ++end;
        const uint8_t* lo = base_ + std::max<uint64_t>(at << 12, from);
        const uint8_t* hi = base_ + std::min<uint64_t>(end << 12, to);
        for (const uint8_t* p = lo; out.size() < max;) {
            p = std::search(p, hi, search);
            if (p == hi) break;
            out.push_back(static_cast<uint32_t>(p - base_));
            ++p;
        }
        at = end;
    }
    return out;
}

uint32_t GuestMemory::alloc_hle(uint32_t size, uint32_t align) {
    uint32_t a = (hle_next_ + align - 1) & ~(align - 1);
    if (a + size > layout::kHleEnd || a + size < a) throw std::runtime_error("HLE guest object area exhausted");
    hle_next_ = a + size;
    return a;
}

void GuestMemory::watch(uint32_t addr, uint32_t size, uint32_t tag) {
    const uint32_t begin = (addr + 0xFFFu) & ~0xFFFu, end = (addr + size) & ~0xFFFu;
    if (end <= begin) return;
    std::lock_guard g(g_watch_m);
    for (const Watch& w : g_watches)
        if (w.begin < end && begin < w.end) return;
    if (mprotect(base_ + begin, end - begin, PROT_NONE) != 0) return;
    g_watches.push_back({begin, end, tag});
}

void GuestMemory::install_fault_reporter() { g_report = true; }
