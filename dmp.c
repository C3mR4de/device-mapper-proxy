#include <linux/module.h>
#include <linux/init.h>
#include <linux/bio.h>
#include <linux/device-mapper.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>

#define DM_MSG_PREFIX "dmp"

// Структура для хранения статистики
struct dmp_stats
{
    atomic64_t read_reqs;
    atomic64_t write_reqs;
    atomic64_t read_bytes;
    atomic64_t write_bytes;
};

// Структура для устройства
struct dmp_device
{
    struct dm_dev*   dev;
    struct dmp_stats stats;
};

// Глобальная структура модуля
static struct
{
    struct kobject*  kobj;
    struct kobject*  stat_kobj;
    struct dmp_stats stats;
}
dmp_mod;

// Операции Device Mapper
static int dmp_map(struct dm_target* ti, struct bio* bio)
{
    struct dmp_device* dmp_dev = ti->private;
    struct dmp_stats*  stats   = &dmp_dev->stats;
    unsigned int       bytes   = bio->bi_iter.bi_size;

    // Обновляем статистику в зависимости от операции
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

// Инициализация устройства (конструктор)
static int dmp_ctr(struct dm_target* ti, unsigned int argc, char** argv)
{
    struct dmp_device* dmp_dev;
    int ret;

    if (argc != 1)
    {
        ti->error = "Invalid argument count";
        return -EINVAL;
    }

    // Выделение памяти в пространстве ядра
    dmp_dev = kmalloc(sizeof(struct dmp_device), GFP_KERNEL);

    if (!dmp_dev)
    {
        ti->error = "Cannot allocate context";
        return -ENOMEM;
    }

    // Инициализация статистики устройства
    atomic64_set(&dmp_dev->stats.read_reqs, 0);
    atomic64_set(&dmp_dev->stats.write_reqs, 0);
    atomic64_set(&dmp_dev->stats.read_bytes, 0);
    atomic64_set(&dmp_dev->stats.write_bytes, 0);

    // Добавление нового блочного устройства
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

// Деинициализация устройства (деструктор)
static void dmp_dtr(struct dm_target* ti)
{
    struct dmp_device* dmp_dev = ti->private;
    dm_put_device(ti, dmp_dev->dev); // Удаление блочного устройства
    kfree(dmp_dev);                  // Очистка памяти
}

// Определение target type
static struct target_type dmp_target =
{
    .name     = "dmp",
    .version  = {1, 0, 0},
    .features = DM_TARGET_NOWAIT,
    .module   = THIS_MODULE,
    .ctr      = dmp_ctr,
    .dtr      = dmp_dtr,
    .map      = dmp_map,
};

// Sysfs интерфейс для чтения статистики (сбор, подсчёт и возврат в форматированном виде)
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

// Атрибут kobject (только для чтения)
static struct kobj_attribute volumes_attr = __ATTR_RO(volumes);

// Инициализация модуля
static int __init dmp_init(void)
{
    int ret;

    // Инициализация статистики модуля
    atomic64_set(&dmp_mod.stats.read_reqs, 0);
    atomic64_set(&dmp_mod.stats.write_reqs, 0);
    atomic64_set(&dmp_mod.stats.read_bytes, 0);
    atomic64_set(&dmp_mod.stats.write_bytes, 0);

    // Создание структуры kobject для ведения статистики
    dmp_mod.kobj = &THIS_MODULE->mkobj.kobj;
    dmp_mod.stat_kobj = kobject_create_and_add("stat", dmp_mod.kobj);

    if (!dmp_mod.stat_kobj)
    {
        return -ENOMEM;
    }

    // Создание файла атрибутов sysfs
    ret = sysfs_create_file(dmp_mod.stat_kobj, &volumes_attr.attr);

    if (ret)
    {
        kobject_put(dmp_mod.stat_kobj);
        return ret;
    }

    // Регистрация устройства для device mapper
    ret = dm_register_target(&dmp_target);

    if (ret)
    {
        sysfs_remove_file(dmp_mod.stat_kobj, &volumes_attr.attr);
        kobject_put(dmp_mod.stat_kobj);
        return ret;
    }

    return 0;
}

// Деинициализация модуля
static void __exit dmp_exit(void)
{
    dm_unregister_target(&dmp_target);                        // Снятие регистрации устройства device mapper
    sysfs_remove_file(dmp_mod.stat_kobj, &volumes_attr.attr); // Удаление файла атрибутов sysfs
    kobject_put(dmp_mod.stat_kobj);                           // Удаление структуры kobject для ведения статистики
}

module_init(dmp_init);
module_exit(dmp_exit);

MODULE_AUTHOR("Vadim Novikov");
MODULE_DESCRIPTION("Device Mapper Proxy");
MODULE_LICENSE("GPL");

