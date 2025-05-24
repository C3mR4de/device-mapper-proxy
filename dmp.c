#include <linux/module.h>
#include <linux/init.h>
#include <linux/bio.h>
#include <linux/device-mapper.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>

#define DM_MSG_PREFIX "dmp"

struct dmp_stats
{
    atomic64_t read_reqs;
    atomic64_t write_reqs;
    atomic64_t read_bytes;
    atomic64_t write_bytes;
};

struct dmp_device
{
    struct dm_dev*   dev;
    struct dmp_stats stats;
};

static struct
{
    struct kobject*  kobj;
    struct kobject*  stat_kobj;
    struct dmp_stats stats;
}
dmp_mod;

static int dmp_map(struct dm_target* ti, struct bio* bio)
{
    struct dmp_device* dmp_dev = ti->private;
    struct dmp_stats*  stats   = &dmp_dev->stats;
    unsigned int       bytes   = bio->bi_iter.bi_size;

    switch (bio_op(bio))
    {
	    case REQ_OP_READ:

		    if (bio->bi_opf & REQ_RAHEAD)
            {
			    return DM_MAPIO_KILL;
		    }

            atomic64_inc(&stats->read_reqs);
            atomic64_add(bytes, &stats->read_bytes);
            atomic64_inc(&stats->write_reqs);
            atomic64_add(bytes, &stats->write_bytes);

            break;

    	case REQ_OP_WRITE:
    	case REQ_OP_DISCARD:

            atomic64_inc(&dmp_mod.stats.read_reqs);
            atomic64_add(bytes, &dmp_mod.stats.read_bytes);
            atomic64_inc(&dmp_mod.stats.write_reqs);
            atomic64_add(bytes, &dmp_mod.stats.write_bytes);

		    break;

	    default:
		    return DM_MAPIO_KILL;
	}

	bio_endio(bio);

    return DM_MAPIO_SUBMITTED;
}

static int dmp_ctr(struct dm_target* ti, unsigned int argc, char** argv)
{
    struct dmp_device* dmp_dev;
    int ret;

    if (argc != 1)
    {
        ti->error = "Invalid argument count";
        return -EINVAL;
    }

    dmp_dev = kmalloc(sizeof(*dmp_dev), GFP_KERNEL);

    if (!dmp_dev)
    {
        ti->error = "Cannot allocate context";
        return -ENOMEM;
    }

    atomic64_set(&dmp_dev->stats.read_reqs, 0);
    atomic64_set(&dmp_dev->stats.write_reqs, 0);
    atomic64_set(&dmp_dev->stats.read_bytes, 0);
    atomic64_set(&dmp_dev->stats.write_bytes, 0);

    ret = dm_get_device(ti, argv[0], dm_table_get_mode(ti->table), &dmp_dev->dev);

    if (ret)
    {
        ti->error = "Device lookup failed";
        kfree(dmp_dev);
        return ret;
    }

    ti->private = dmp_dev;
    return 0;
}

static void dmp_dtr(struct dm_target* ti)
{
    struct dmp_device* dmp_dev = ti->private;
    dm_put_device(ti, dmp_dev->dev);
    kfree(dmp_dev);
}

static void dmp_io_hints(struct dm_target* ti, struct queue_limits* limits)
{
	limits->max_hw_discard_sectors = UINT_MAX;
	limits->discard_granularity    = 512;
}

static struct target_type dmp_target =
{
    .name     = "dmp",
    .version  = {1, 0, 0},
    .features = DM_TARGET_NOWAIT,
    .module   = THIS_MODULE,
    .ctr      = dmp_ctr,
    .dtr      = dmp_dtr,
    .map      = dmp_map,
    .io_hints = dmp_io_hints,
};

static ssize_t volumes_show(struct kobject* kobj, struct kobj_attribute* attr, char* buf)
{
    struct dmp_stats* stats = &dmp_mod.stats;

    u64 read_reqs   = atomic64_read(&stats->read_reqs);
    u64 write_reqs  = atomic64_read(&stats->write_reqs);
    u64 total_reqs  = read_reqs + write_reqs;
    u64 read_bytes  = atomic64_read(&stats->read_bytes);
    u64 write_bytes = atomic64_read(&stats->write_bytes);

    u64 avg_read_size  = read_reqs  ? read_bytes / read_reqs : 0;
    u64 avg_write_size = write_reqs ? write_bytes / write_reqs : 0;
    u64 avg_total_size = total_reqs ? (read_bytes + write_bytes) / total_reqs : 0;

    return scnprintf(buf, PAGE_SIZE,
        "read:\n"
        "    reqs: %llu\n"
        "    avg size: %llu\n"
        "write:\n"
        "    reqs: %llu\n"
        "    avg size: %llu\n"
        "total:\n"
        "    reqs: %llu\n"
        "    avg size: %llu\n",
        read_reqs, avg_read_size,
        write_reqs, avg_write_size,
        total_reqs, avg_total_size);
}

static struct kobj_attribute volumes_attr = __ATTR_RO(volumes);

static int __init dmp_init(void)
{
    int ret;

    atomic64_set(&dmp_mod.stats.read_reqs, 0);
    atomic64_set(&dmp_mod.stats.write_reqs, 0);
    atomic64_set(&dmp_mod.stats.read_bytes, 0);
    atomic64_set(&dmp_mod.stats.write_bytes, 0);

    dmp_mod.kobj = &THIS_MODULE->mkobj.kobj;
    dmp_mod.stat_kobj = kobject_create_and_add("stat", dmp_mod.kobj);

    if (!dmp_mod.stat_kobj)
    {
        return -ENOMEM;
    }

    ret = sysfs_create_file(dmp_mod.stat_kobj, &volumes_attr.attr);

    if (ret)
    {
        kobject_put(dmp_mod.stat_kobj);
        return ret;
    }

    ret = dm_register_target(&dmp_target);

    if (ret)
    {
        sysfs_remove_file(dmp_mod.stat_kobj, &volumes_attr.attr);
        kobject_put(dmp_mod.stat_kobj);
        return ret;
    }

    return 0;
}

static void __exit dmp_exit(void)
{
    dm_unregister_target(&dmp_target);
    sysfs_remove_file(dmp_mod.stat_kobj, &volumes_attr.attr);
    kobject_put(dmp_mod.stat_kobj);
}

module_init(dmp_init);
module_exit(dmp_exit);

MODULE_AUTHOR("Vadim Novikov");
MODULE_DESCRIPTION("Device Mapper Proxy");
MODULE_LICENSE("GPL");
