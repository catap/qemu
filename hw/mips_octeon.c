/*
 * QEMU/MIPS pseudo-board
 *
 * emulates a simple machine with ISA-like bus.
 * ISA IO space mapped to the 0x14000000 (PHYS) and
 * ISA memory at the 0x10000000 (PHYS, 16Mb in size).
 * All peripherial devices are attached to this "bus" with
 * the standard PC ISA addresses.
*/

#include "hw.h"
#include "mips.h"
#include "mips_cpudevs.h"
#include "pc.h"
#include "isa.h"
#include "net.h"
#include "sysemu.h"
#include "boards.h"
#include "flash.h"
#include "qemu-log.h"
#include "mips-bios.h"
#include "ide.h"
#include "loader.h"
#include "elf.h"
#include "mc146818rtc.h"
#include "blockdev.h"
#include "exec-memory.h"
#include "pci.h"
#include "sysbus.h"

#define MAX_IDE_BUS 2
int ayaz=0;
int ayaz2=0;
CPUState * envArray[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static const int ide_iobase[2] = { 0x1f0, 0x170 };
static const int ide_iobase2[2] = { 0x3f6, 0x376 };
static const int ide_irq[2] = { 14, 15 };
static void cpu_request_exit(void *opaque, int irq, int level)  //added by ayaz on may 19
{
    CPUState *env = cpu_single_env;

    if (env && level) {
        cpu_exit(env);
    }
}

static int mips_octeon_sysbus_device_init(SysBusDevice *sysbusdev)
{
    return 0;
}

typedef struct {
    SysBusDevice busdev;
    qemu_irq *i8259;
} OcteonState;

static SysBusDeviceInfo mips_octeon_device = {
    .init = mips_octeon_sysbus_device_init,
    .qdev.name  = "mips-octeon",
    .qdev.size  = sizeof(OcteonState),
    .qdev.props = (Property[]) {
        DEFINE_PROP_END_OF_LIST(),
    }
};

static void mips_octeon_device_init(void)
{
    sysbus_register_withprop(&mips_octeon_device);
}

device_init(mips_octeon_device_init);




static int load_kernel_tailored(const char* kernelFile, MemoryRegion *ram);

static ISADevice *pit; /* PIT i8254 */
/* i8254 PIT is attached to the IRQ0 at PIC i8259 */

static struct _loaderparams {
    int ram_size;
    const char *kernel_filename;
    const char *kernel_cmdline;
    const char *initrd_filename;
} loaderparams;
extern void
e1000_mmio_write(void *opaque, target_phys_addr_t addr, uint64_t val,
                 unsigned size);

static void mips_qemu_write (void *opaque, target_phys_addr_t addr,
                             uint64_t val, unsigned size)
{  // printf("\n mips_qemu_write  ++++++++++++ : mqw:addr:0x%llx \n",addr);
    if( 1 == ayaz2 )
     e1000_mmio_write(&opaque,addr,val,size);
    
    else if ((addr & 0xffff) == 0 && val == 42)
        qemu_system_reset_request ();
    else if ((addr & 0xffff) == 4 && val == 42)
        qemu_system_shutdown_request ();
}
extern uint64_t 
e1000_mmio_read(void *opaque, target_phys_addr_t addr, unsigned size);

static uint64_t mips_qemu_read (void *opaque, target_phys_addr_t addr,
                                unsigned size)
{   //printf("\n mqr: address:0x%llx  \n",addr);
	if( 1 == ayaz )
	return e1000_mmio_read(&opaque,addr,size);
    else
    {
    return 0;  //commented by ayaz
 }
}

static const MemoryRegionOps mips_qemu_ops = {
    .read = mips_qemu_read,
    .write = mips_qemu_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};


/* Network support */
static void network_init(void)
{
    int i;

    for(i = 0; i < nb_nics; i++) {
        NICInfo *nd = &nd_table[i];
        const char *default_devaddr = NULL;

        if (i == 0 && (!nd->model || strcmp(nd->model, "e1000") == 0))
            /* The malta board has a PCNet card using PCI SLOT 11 */
            default_devaddr = "0b";

        pci_nic_init_nofail(nd, "e1000", default_devaddr);
    }
}


typedef struct ResetData {
    CPUState *env;
    uint64_t vector;
} ResetData;



static void main_cpu_reset(void *opaque)
{
    ResetData *s = (ResetData *)opaque;
    CPUState *env = s->env;

    cpu_reset(env);
    env->active_tc.PC = s->vector;
}

static const int sector_len = 32 * 1024;
static
void mips_r4k_init (ram_addr_t ram_size,
                    const char *boot_device,
                    const char *kernel_filename, const char *kernel_cmdline,
                    const char *initrd_filename, const char *cpu_model)
{
  
    char *filename;
    MemoryRegion *address_space_mem = get_system_memory();
    MemoryRegion *ram = g_new(MemoryRegion, 1);
    MemoryRegion *ram1 = g_new(MemoryRegion, 1);
    MemoryRegion *ram2 = g_new(MemoryRegion, 1);
    MemoryRegion *cvmseg = g_new(MemoryRegion, 1);

          
    MemoryRegion *bios, *bios_alias,*kernel_load;//, *bios_octeon;
    MemoryRegion *iomem = g_new(MemoryRegion, 1);

    //.....................................................
    PCIBus *pci_bus;
    DeviceState *dev = qdev_create(NULL, "mips-octeon");
    OcteonState *s = DO_UPCAST(OcteonState, busdev.qdev, dev);
    int piix4_devfn;
    //.....................................................

    int bios_size;
    CPUState *env;
    ResetData *reset_info;
    int i;
    qemu_irq *i8259;
    DriveInfo *hd[MAX_IDE_BUS * MAX_IDE_DEVS];
    DriveInfo *dinfo;
    int be;

    /* init CPUs */
    if (cpu_model == NULL) {
#ifdef TARGET_MIPS64
        cpu_model = "octeon";
#else
        cpu_model = "24Kf";
#endif
    }
	   for (i = 0; i < smp_cpus; i++) {
		env = cpu_init(cpu_model);
		if (!env) {
		fprintf(stderr, "Unable to find CPU definition\n");
		exit(1);
		}
		reset_info = g_malloc0(sizeof(ResetData));
		reset_info->env = env;
		reset_info->vector = env->active_tc.PC;
		cpu_mips_irq_init_cpu(env);
		cpu_mips_clock_init(env);
		qemu_register_reset(main_cpu_reset, reset_info);
		envArray[i]=env; //store env pointer in array to access in softmmu_template.h file: smp issue
		}
	env = first_cpu;

////************The following is the most important region*********************************************************
 

     memory_region_init_ram(ram, NULL, "DR0 DRAM", 0x010000000ULL);
        memory_region_add_subregion(address_space_mem, 0x0ULL, ram);

 
        memory_region_init_ram(ram1, NULL, "DR1 DRAM", 0x010000000ULL);
        memory_region_add_subregion(address_space_mem, 0x410000000ULL, ram1);

        memory_region_init_ram(ram2, NULL, "DR2 DRAM", 0x020000000ULL);
        memory_region_add_subregion(address_space_mem, 0x020000000ULL, ram2);
   
        memory_region_init_ram(cvmseg, NULL, "cvm segment", 0x1FFFULL);
        memory_region_add_subregion(address_space_mem,0x510000000ULL , cvmseg);



    memory_region_init_io(iomem, &mips_qemu_ops, NULL, "mips-qemu", 0x10000);
    memory_region_add_subregion(address_space_mem, 0x1fbf0000, iomem);
    

    // printf("\n address space mem:%d \n",pci_bus->address_space_mem);
     //printf("\n address space io:%d \n",pci_bus->address_space_io);
     
     
   /* Try to load a BIOS image. If this fails, we continue regardless,
       but initialize the hardware ourselves. When a kernel gets
       preloaded we also initialize the hardware, since the BIOS wasn't
       run. */
    if (bios_name == NULL)
        bios_name = BIOS_FILENAME;
    filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, bios_name);
    if (filename) {
        bios_size = get_image_size(filename);
    } else {
        bios_size = -1;
    }
#ifdef TARGET_WORDS_BIGENDIAN
    be = 1;
#else
    be = 0;
#endif
    if ((bios_size > 0) && (bios_size <= BIOS_SIZE)) {
        fprintf(stderr, "running bios");
        bios = g_new(MemoryRegion, 1);
        bios_alias = g_new(MemoryRegion, 1);


        memory_region_init_ram(bios, NULL, "mips_r4k.bios", BIOS_SIZE);
        memory_region_init_ram(bios_alias, NULL, "bios.1fc", BIOS_SIZE);


        memory_region_set_readonly(bios, true);
        memory_region_add_subregion(get_system_memory(), 0x1e000000LL, bios);
        memory_region_add_subregion(get_system_memory(), 0x1fc00000LL, bios_alias);


        load_image_targphys(filename, 0x1e000000, BIOS_SIZE);

        load_image_targphys(filename, 0x1fc00000, BIOS_SIZE);


        if(kernel_filename)
        {
        int64_t k_size=get_image_size(kernel_filename);

        kernel_load= g_new(MemoryRegion, 1);
        memory_region_init_ram(kernel_load, NULL, "kernel_load", k_size);
        memory_region_add_subregion(get_system_memory(),0x10000000,kernel_load);
        uint64_t *exp_base = memory_region_get_ram_ptr(kernel_load);
    uint64_t r = bswap64(k_size);
        memcpy(exp_base,&r,sizeof(r));

      load_kernel_tailored(kernel_filename, ram);


}
        else
            fprintf(stderr,"\nNo kernel file found to be loaded\n");
            //load_image_targphys(filename, 0x100001fc00000, BIOS_SIZE);
    } else if ((dinfo = drive_get(IF_PFLASH, 0, 0)) != NULL) {
        uint32_t mips_rom = 0x00400000;
        if (!pflash_cfi01_register(0x1fc00000, NULL, "mips_r4k.bios", mips_rom,
                                   dinfo->bdrv, sector_len,
                                   mips_rom / sector_len,
                                   4, 0, 0, 0, 0, be)) {
            fprintf(stderr, "qemu: Error registering flash memory.\n");
    }
    }
    else {
    /* not fatal */
        fprintf(stderr, "qemu: Warning, could not load MIPS bios '%s'\n",
        bios_name);
    }
    if (filename) {
        g_free(filename);
    }

    if (kernel_filename) {
        int64_t entry_bin;
        MemoryRegion *enter = g_new(MemoryRegion, 1);
        memory_region_init_ram(enter, NULL, "entry.boot", sizeof(entry_bin));
           memory_region_add_subregion(address_space_mem, 0x1a000000LL, enter);

uint64_t *base_entry = memory_region_get_ram_ptr(enter);

        loaderparams.ram_size = ram_size;
        loaderparams.kernel_filename = kernel_filename;
        loaderparams.kernel_cmdline = kernel_cmdline;
        loaderparams.initrd_filename = initrd_filename;

        entry_bin= bswap64(entry_bin);
        memcpy(base_entry,&entry_bin,sizeof(entry_bin));

    // Following code is added to write CIU at appropriate place
 	MemoryRegion *ciu = g_new(MemoryRegion, 1);
	memory_region_init_ram(ciu, NULL, "CIU",0x900ULL );
	memory_region_add_subregion(address_space_mem, 0x0001070000000000ULL, ciu);
   
   /* int64_t ciu_fuse;
        MemoryRegion *ciu_fuse_mem_reg = g_new(MemoryRegion, 1);
        memory_region_init_ram(ciu_fuse_mem_reg, NULL, "ciu_fuse", sizeof(ciu_fuse));

        memory_region_add_subregion(address_space_mem, 0x0001070000000728ULL, ciu_fuse_mem_reg);
    ciu_fuse = 0x1; */

 }   
   /* Init CPU internal devices */
    cpu_mips_irq_init_cpu(env);
    cpu_mips_clock_init(env);

    //...................AQ
    qemu_irq *isa_irq;
    isa_irq = qemu_irq_proxy(&s->i8259, 16);

    /* Northbridge */
    pci_bus = gt64120_register(isa_irq);

    /* Southbridge */
    ide_drive_get(hd, MAX_IDE_BUS);

    piix4_devfn = piix4_init(pci_bus, 80);

    s->i8259 = i8259_init(env->irq[2]);
    isa_bus_irqs(s->i8259);

    //pci_piix4_ide_init(pci_bus, hd, piix4_devfn+1);
    pit = pit_init(0x40, 0);
     
    //...................

    /* The PIC is attached to the MIPS CPU INT0 pin */
    ////isa_bus_new(NULL, get_system_io());
    ////i8259 = i8259_init(env->irq[2]);
    ////isa_bus_irqs(i8259);

    ////rtc_init(2000, NULL);
      fprintf(stderr,"\n network_init() will be called now \n");
    network_init();  //added by ayaz 
         qemu_irq *cpu_exit_irq;
   cpu_exit_irq = qemu_allocate_irqs(cpu_request_exit, NULL, 1);  //added by ayaz on 19 may
   // DMA_init(0, cpu_exit_irq);  //added by ayaz on 19 may

    /* Register 64 KB of ISA IO space at 0x14000000 */
    ////isa_mmio_init(0x14000000, 0x00010000);//this is what is required by qemu serial stuff
    isa_mmio_init(0x18000000, 0x00010000);//this is what is required by qemu serial stuff   //changed by ayaz
  // isa_mmio_init(0x1180000000800, 0x00010000);// this is what required by our octeon kernel
 //    as a hack we have put both here
 //   isa_mmio_init(0x1be00cf8, 0x00010000);  //added by ayaz
   ////pit = pit_init(0x40, 0);

    for(i = 0; i < MAX_SERIAL_PORTS; i++) {
        if (serial_hds[i]) {
            serial_isa_init(i, serial_hds[i]);
        }
    }

    //isa_vga_init();

    //if (nd_table[0].vlan)
       // isa_ne2000_init(0x300, 9, &nd_table[0]);

    ide_drive_get(hd, MAX_IDE_BUS);
    for(i = 0; i < MAX_IDE_BUS; i++)
        isa_ide_init(ide_iobase[i], ide_iobase2[i], ide_irq[i],
                     hd[MAX_IDE_DEVS * i],
             hd[MAX_IDE_DEVS * i + 1]);

    isa_create_simple("i8042");
}

static QEMUMachine mips_machine = {
    .name = "octeon",
    .desc = "mips r4k platform",
    .init = mips_r4k_init,
    .max_cpus = 16,
};

static void mips_machine_init(void)
{
    qemu_register_machine(&mips_machine);
  
}



machine_init(mips_machine_init);

 


// Code added by AQ on Nov 3, 2012

static int load_kernel_tailored(const char* kernelFile, MemoryRegion *ram)
{
    int rfd = -1, wfd = -1;
    int kernelFileSize = 0;
    uint8_t *rBuffer = NULL;
    int rc = 0; // read counter
    //1. Open kernel file and find its size


    rfd = open(kernelFile, O_RDONLY|O_BINARY|O_LARGEFILE);
    if (rfd == -1)
    {
        fprintf(stderr, "\n Failed to open Kernel file %s .", kernelFile);
        goto err;
    }

    kernelFileSize = lseek(rfd, 0, SEEK_END);


    rBuffer = malloc(kernelFileSize);
    if(NULL == rBuffer)
    {
        fprintf(stderr, "\n Failed to allocate memory for read buffer.");
        goto err;
    }
    lseek(rfd, 0, SEEK_SET);

    rc = read(rfd, rBuffer, kernelFileSize);

    if(rc != kernelFileSize)
    {
        fprintf(stderr, "\n Failed to read sufficient bytes from the Kernel file.");
        goto err;

    }


    //2. Copy data in guest memory

    rom_add_blob("kernel", rBuffer, kernelFileSize,0x00800000);




    return 0;

    err:
        if(-1 != rfd) close(rfd);
        if(NULL != rBuffer) free(rBuffer);
        if (-1 != wfd) close(wfd);
        return -1;
}

