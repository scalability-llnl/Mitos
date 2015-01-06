#include "perfsmpl.h"

static void *sample_reader_fn(void *args)
{
    perfsmpl *pep = (perfsmpl*)args;

    while(!pep->stop) 
    {
        pep->process_sample_buffer();
    }
}

perfsmpl::perfsmpl()
{
    // Defaults
    mmap_pages = 32;
    sample_period = 10;
    pgsz = sysconf(_SC_PAGESIZE);
    mmap_size = (mmap_pages+1)*pgsz;
    pgmsk = mmap_pages*pgsz-1;

    ret = 0;
    ready = 0;
    stop = 0;

    custom_handler = 0;

    collected_samples = 0;
    lost_samples = 0;
}

perfsmpl::~perfsmpl()
{
    close(fd);
    munmap(mmap_buf,mmap_size);
}

void perfsmpl::init_attr()
{
    // event attr
    memset(&pe, 0, sizeof(struct perf_event_attr));
    pe.size = sizeof(struct perf_event_attr);

    pe.mmap = 1;
    pe.mmap_data = 1;
    pe.comm = 1;
    pe.disabled = 1;
    pe.exclude_user = 0;
    pe.exclude_kernel = 0;
    pe.exclude_hv = 0;
    pe.exclude_idle = 0;
    pe.exclude_host = 0;
    pe.exclude_guest = 1;
    pe.pinned = 0;
    pe.sample_id_all = 0;

    pe.sample_period = 4000;
    pe.freq = 0;

    if(mode == SMPL_MEMORY)
    {
        // TODO: look this up in libpfm
        pe.type = PERF_TYPE_RAW;
        pe.config = 0x5101cd;
        pe.config1 = 3; // ldlat
        pe.sample_type = 
            PERF_SAMPLE_IP | 
            PERF_SAMPLE_CALLCHAIN | 
            PERF_SAMPLE_ID | 
            PERF_SAMPLE_STREAM_ID | 
            PERF_SAMPLE_TIME | 
            PERF_SAMPLE_TID | 
            PERF_SAMPLE_PERIOD | 
            PERF_SAMPLE_CPU | 
            PERF_SAMPLE_ADDR | 
            PERF_SAMPLE_WEIGHT | 
            PERF_SAMPLE_DATA_SRC;
        pe.precise_ip = 2;
    }

    if(mode == SMPL_INSTRUCTIONS)
    {
        pe.type = PERF_TYPE_HARDWARE;
        pe.config = PERF_COUNT_SW_DUMMY;
        pe.sample_type = 
            PERF_SAMPLE_IP | 
            PERF_SAMPLE_CALLCHAIN | 
            PERF_SAMPLE_ID | 
            PERF_SAMPLE_STREAM_ID | 
            PERF_SAMPLE_TIME | 
            PERF_SAMPLE_TID | 
            PERF_SAMPLE_PERIOD | 
            PERF_SAMPLE_CPU;
    }
}

int perfsmpl::init_perf()
{
    // Create attr according to sample mode
    // Setup
    fd = syscall(__NR_perf_event_open, &pe,0,-1,-1,0);

    if(fd == -1) 
    {
       std::cerr << "Error from perf_event_open syscall" << std::endl;
       ready=0;
       return -1;
    }

    // Create mmap buffer for samples
    mmap_buf = (struct perf_event_mmap_page*)
        mmap(NULL, mmap_size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);

    if(mmap_buf == MAP_FAILED) 
    {
       std::cerr << "Error mmap-ing buffer " << std::endl;
       ready = 0;
       return -1;
    }

    ready = 1;

    return 0;
}

int perfsmpl::prepare()
{
    init_attr();

    ret = init_perf();

    if(ret != 0)
    {
        ready = 0;
        return ret;
    }

    ready = 1;

    return 0;
}

int perfsmpl::init_sample_reader()
{
    return pthread_create(&sample_reader_thr,NULL,sample_reader_fn,(void*)this);
}

int perfsmpl::begin_sampler()
{
    if(!ready)
    {
        std::cerr << "Not ready to begin sampling!\n" << std::endl;
        std::cerr << "Did you prepare()?\n" << std::endl;
        return -1;
    }

    ret = init_sample_reader();

    if(ret)
    {
        std::cerr << "Couldn't initialize sample handler thread, aborting!\n";
        std::cerr << std::endl;
        return ret;
    }

    ioctl(fd, PERF_EVENT_IOC_RESET, 0);
    ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);

    return ret;
}

void perfsmpl::end_sampler()
{
    stop = 1;
    pthread_join(sample_reader_thr,NULL);

    ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);
    read(fd, &counter_value, sizeof(uint64_t));

    process_sample_buffer(); // flush out remaining samples
}

int perfsmpl::process_single_sample(struct perf_event_mmap_page *mmap_buf)
{
    // Read a sample from the mmap buf
    if(ret)
    {
        std::cerr << "Can't read mmap buffer!\n" << std::endl;
        return -1;
    }

    collected_samples++;

    // Create and fill up a new sample
    perf_event_sample *sample = new perf_event_sample();
    sample->parent = this;

    if(has_attribute(PERF_SAMPLE_IP))
    {
        read_mmap_buffer(mmap_buf,(char*)&sample->ip,sizeof(uint64_t));
    }
    
    if(has_attribute(PERF_SAMPLE_TID))
    {
        read_mmap_buffer(mmap_buf,(char*)&sample->pid,sizeof(uint32_t));
        read_mmap_buffer(mmap_buf,(char*)&sample->tid,sizeof(uint32_t));
    }

    if(has_attribute(PERF_SAMPLE_TIME))
    {
        read_mmap_buffer(mmap_buf,(char*)&sample->time,sizeof(uint64_t));
    }

    if(has_attribute(PERF_SAMPLE_ADDR))
    {
        read_mmap_buffer(mmap_buf,(char*)&sample->addr,sizeof(uint64_t));
    }

    if(has_attribute(PERF_SAMPLE_ID))
    {
        read_mmap_buffer(mmap_buf,(char*)&sample->id,sizeof(uint64_t));
    }

    if(has_attribute(PERF_SAMPLE_STREAM_ID))
    {
        read_mmap_buffer(mmap_buf,(char*)&sample->stream_id,sizeof(uint64_t));
    }

    if(has_attribute(PERF_SAMPLE_CPU))
    {
        read_mmap_buffer(mmap_buf,(char*)&sample->cpu,sizeof(uint32_t));
        read_mmap_buffer(mmap_buf,(char*)&sample->res,sizeof(uint32_t));
    }

    if(has_attribute(PERF_SAMPLE_PERIOD))
    {
        read_mmap_buffer(mmap_buf,(char*)&sample->period,sizeof(uint64_t));
    }

    if(has_attribute(PERF_SAMPLE_CALLCHAIN))
    {
        read_mmap_buffer(mmap_buf,(char*)&sample->nr,sizeof(uint64_t));
        sample->ips = (uint64_t*)malloc(sample->nr*sizeof(uint64_t));
        read_mmap_buffer(mmap_buf,(char*)sample->ips,sample->nr*sizeof(uint64_t));
    }

    if(has_attribute(PERF_SAMPLE_WEIGHT))
    {
        read_mmap_buffer(mmap_buf,(char*)&sample->weight,sizeof(uint64_t));
    }

    if(has_attribute(PERF_SAMPLE_DATA_SRC))
    {
        read_mmap_buffer(mmap_buf,(char*)&sample->data_src,sizeof(uint64_t));
    }

    if(custom_handler)
    {
        handler(sample,NULL);
    }

    return ret;
}

int perfsmpl::process_sample_buffer()
{
    struct perf_event_header ehdr;
    int ret;

    for(;;) {
        ret = read_mmap_buffer(mmap_buf,(char*)&ehdr,sizeof(ehdr));
        if(ret)
            return 0; // no more samples

        switch(ehdr.type) {
            case PERF_RECORD_SAMPLE:
                process_single_sample(mmap_buf);
                break;
            case PERF_RECORD_EXIT:
                process_exit_sample(mmap_buf);
                break;
            case PERF_RECORD_LOST:
                process_lost_sample(mmap_buf);
                break;
            case PERF_RECORD_THROTTLE:
                process_freq_sample(mmap_buf);
                break;
            case PERF_RECORD_UNTHROTTLE:
                process_freq_sample(mmap_buf);
                break;
            default:
                std::cerr << "Unknown sample type ";
                std::cerr << ehdr.type << std::endl;
                skip_mmap_buffer(mmap_buf,sizeof(ehdr));
        }
    }
}

int perfsmpl::read_mmap_buffer(struct perf_event_mmap_page *mmap_buf, char *out, size_t sz)
{
	char *data;
	unsigned long tail;
	size_t avail_sz, m, c;
	
	data = ((char *)mmap_buf)+sysconf(_SC_PAGESIZE);
	tail = mmap_buf->data_tail & pgmsk;
	avail_sz = mmap_buf->data_head - mmap_buf->data_tail;
	if (sz > avail_sz)
		return -1;
	c = pgmsk + 1 -  tail;
	m = c < sz ? c : sz;
	memcpy(out, data+tail, m);
	if ((sz - m) > 0)
		memcpy(out+m, data, sz - m);
	mmap_buf->data_tail += sz;

	return 0;
}

void perfsmpl::skip_mmap_buffer(struct perf_event_mmap_page *mmap_buf, size_t sz)
{
    if ((mmap_buf->data_tail + sz) > mmap_buf->data_head)
        sz = mmap_buf->data_head - mmap_buf->data_tail;

    mmap_buf->data_tail += sz;
}

void perfsmpl::process_lost_sample(struct perf_event_mmap_page *mmap_buf)
{
	struct { uint64_t id, lost; } lost;
	const char *str;

	ret = read_mmap_buffer(mmap_buf,(char*)&lost,sizeof(lost));

	lost_samples += lost.lost;
}

void perfsmpl::process_exit_sample(struct perf_event_mmap_page *mmap_buf)
{
	struct { pid_t pid, ppid, tid, ptid; } grp;
	int ret;

	ret = read_mmap_buffer(mmap_buf,(char*)&grp,sizeof(grp));
}

void perfsmpl::process_freq_sample(struct perf_event_mmap_page *mmap_buf)
{
	struct { uint64_t time, id, stream_id; } thr;
	int ret;

	ret = read_mmap_buffer(mmap_buf,(char*)&thr, sizeof(thr));
}
