#include <stddef.h>
#include <stdint.h>

extern "C" {
    void* malloc(size_t size);
    void free(void* ptr);

    // C-String und Speicherfunktionen
    int string_length(const char* str);
    void string_copy(char* dest, const char* src);
    bool string_compare(const char* s1, const char* s2);
    void meA1et(void* ptr, uint8_t value, uint32_t size);
    void memcpy(void* dest, const void* src, uint32_t size);
}

// Minimal heap implementation (for example only)
static uint8_t kernel_heap[1024 * 1024]; // 1 MB heap
static size_t heap_top = 0;

extern "C" void* malloc(size_t size) {
    if (heap_top + size >= sizeof(kernel_heap)) return nullptr;
    void* ptr = &kernel_heap[heap_top];
    heap_top += size;
    return ptr;
}

extern "C" void free(void*) {
    // no-op for now
}

// C++ operators
void* operator new(size_t size) { return malloc(size); }
void* operator new[](size_t size) { return malloc(size); }
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }


// Multiboot-Header-Struktur
struct multiboot_header {
    uint32_t magic;
    uint32_t flags;
    uint32_t checksum;
};

const uint32_t MULTIBOOT_MAGIC = 0x1BADB002;
const uint32_t MULTIBOOT_FLAGS = 0x00000003;
const uint32_t MULTIBOOT_CHECKSUM = -(MULTIBOOT_MAGIC + MULTIBOOT_FLAGS);

__attribute__((section(".multiboot_header"))) struct multiboot_header header = {
    .magic = MULTIBOOT_MAGIC,
    .flags = MULTIBOOT_FLAGS,
    .checksum = MULTIBOOT_CHECKSUM
};

// ============================================================================
// I/O PORT FUNKTIONEN
// ============================================================================

extern "C" uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

extern "C" void outb(uint16_t port, uint8_t val) {
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

extern "C" void insl(uint16_t port, void* addr, uint32_t count) {
    asm volatile("cld; rep insl" : "+D"(addr), "+c"(count) : "d"(port) : "memory");
}

extern "C" void outsl(uint16_t port, const void* addr, uint32_t count) {
    asm volatile("cld; rep outsl" : "+S"(addr), "+c"(count) : "d"(port) : "memory");
}

// Verbesserte Delay-Funktion (wichtig!)
void delay(uint32_t count) {
    for (volatile uint32_t i = 0; i < count * 1000000; ++i) {
        asm volatile("nop");
    }
}

// Kürzere Delay für I/O-Operationen
void io_wait() {
    for (volatile int i = 0; i < 4; ++i) {
        inb(0x80); // Port 0x80 für I/O-Delay
    }
}

// ============================================================================
// GRUNDLEGENDE VGA FUNKTIONEN
// ============================================================================

void print_char(int row, int col, char character, uint16_t color) {
    if (row < 0 || row >= 25 || col < 0 || col >= 80) return;
    uint16_t* video_memory = (uint16_t*)0xb8000;
    int offset = row * 80 + col;
    video_memory[offset] = (color << 8) | character;
}

void print_string(int row, int col, const char* str, uint16_t color) {
    int i = 0;
    while (str[i] != '\0') {
        print_char(row, col + i, str[i], color);
        i++;
    }
}

void print_string_centered(int row, const char* str, uint16_t color) {
    int len = 0;
    while (str[len] != '\0') len++;
    int col = (80 - len) / 2;
    print_string(row, col, str, color);
}

void clear_screen(uint16_t color) {
    uint16_t* video_memory = (uint16_t*)0xb8000;
    for (int i = 0; i < 80 * 25; ++i) {
        video_memory[i] = (color << 8) | ' ';
    }
}

// ============================================================================
// C-STRING UND SPEICHER FUNKTIONEN (JETZT EXTERN C)
// ============================================================================
extern "C" {
    int string_length(const char* str) {
        int len = 0;
        while (str[len] != '\0') len++;
        return len;
    }

    void string_copy(char* dest, const char* src) {
        int i = 0;
        while (src[i] != '\0') {
            dest[i] = src[i];
            i++;
        }
        dest[i] = '\0';
    }

    bool string_compare(const char* s1, const char* s2) {
        int i = 0;
        while (s1[i] != '\0' && s2[i] != '\0') {
            if (s1[i] != s2[i]) return false;
            i++;
        }
        return s1[i] == s2[i];
    }

    void meA1et(void* ptr, uint8_t value, uint32_t size) {
        uint8_t* p = (uint8_t*)ptr;
        for (uint32_t i = 0; i < size; i++) {
            p[i] = value;
        }
    }

    void memcpy(void* dest, const void* src, uint32_t size) {
        uint8_t* d = (uint8_t*)dest;
        const uint8_t* s = (const uint8_t*)src;
        for (uint32_t i = 0; i < size; i++) {
            d[i] = s[i];
        }
    }
} // Ende extern "C"

const char* get_filename_ext(const char *filename) {
    const char *dot = nullptr;
    while (*filename) {
        if (*filename == '.') dot = filename;
        filename++;
    }
    return dot ? dot + 1 : "";
}


// ============================================================================
// VIRTUELLER (RAM-DISK) DISK DRIVER
// ============================================================================

// Dummy-Struktur, um FAT-Treiber-Signatur anzupassen
struct ATADevice {
    uint16_t io_base;
    uint16_t control_base;
    uint8_t drive; 
    bool present;
    uint32_t size_sectors;
};

// Es gibt nur noch die RAM-Disk
ATADevice* active_disk = nullptr; // Zeigt auf die RAM-Disk

// +++ BEGINN RAM-DISK IMPLEMENTIERUNG +++
const size_t RAMDISK_SIZE_BYTES = 800 * 1024; // 800KB
const size_t RAMDISK_SIZE_SECTORS = RAMDISK_SIZE_BYTES / 512;
uint8_t* ramdisk_storage = nullptr; // Zeiger auf den Speicher der RAM-Disk
ATADevice ramdisk_device; // Ein virtuelles ATADevice für die RAM-Disk
#define RAMDISK_MAGIC_IO 0xDEAD // Eindeutige ID statt I/O-Port
#define SECTOR_SIZE 512 // Definiert hier, da es global genutzt wird
// +++ ENDE RAM-DISK IMPLEMENTIERUNG +++


bool ata_read_sector(ATADevice* dev, uint32_t lba, uint8_t* buffer) {
    // +++ RAM-DISK LESE-LOGIK +++
    if (dev->io_base == RAMDISK_MAGIC_IO) {
        if (!dev->present || lba >= dev->size_sectors) return false;
        
        uint32_t offset = lba * SECTOR_SIZE;
        memcpy(buffer, &ramdisk_storage[offset], SECTOR_SIZE);
        return true;
    }
    // +++ ENDE RAM-DISK +++

    // Kein Hardware-Support mehr
    return false;
}

bool ata_write_sector(ATADevice* dev, uint32_t lba, const uint8_t* buffer) {
    // +++ RAM-DISK SCHREIB-LOGIK +++
    if (dev->io_base == RAMDISK_MAGIC_IO) {
        if (!dev->present || lba >= dev->size_sectors) return false;
        
        uint32_t offset = lba * SECTOR_SIZE;
        memcpy(&ramdisk_storage[offset], buffer, SECTOR_SIZE);
        return true;
    }
    // +++ ENDE RAM-DISK +++

    // Kein Hardware-Support mehr
    return false;
}

// ============================================================================
// FAT16 DATEISYSTEM
// ============================================================================

#define MAX_FILES 64
#define MAX_FILENAME 12

struct BootSector {
    uint8_t jump[3];
    char oem[8];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t fat_count;
    uint16_t root_entry_count; 
    uint16_t total_sectors_16; 
    uint8_t media_type;
    uint16_t sectors_per_fat_16; 
    uint16_t sectors_per_track;
    uint16_t head_count;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    // FAT16/12 specific
    uint8_t drive_number;
    uint8_t reserved1;
    uint8_t boot_signature;
    uint32_t volume_id;
    char volume_label[11];
    char fs_type[8];
} __attribute__((packed));

struct DirEntry {
    char filename[11];
    uint8_t attributes;
    uint8_t reserved;
    uint8_t creation_time_A1;
    uint16_t creation_time;
    uint16_t creation_date;
    uint16_t last_access_date;
    uint16_t first_cluster_high;
    uint16_t last_modified_time;
    uint16_t last_modified_date;
    uint16_t first_cluster_low;
    uint32_t file_size;
} __attribute__((packed));

struct FileEntry {
    char name[MAX_FILENAME + 1];
    uint32_t size;
    bool is_directory;
    uint32_t first_cluster;
};

FileEntry file_cache[MAX_FILES];
int file_count = 0;

BootSector boot_sector;
uint8_t sector_buffer[SECTOR_SIZE];
bool fs_mounted = false;

uint32_t fat_begin_lba = 0;
uint32_t cluster_begin_lba = 0;
uint32_t sectors_per_cluster = 0;
uint32_t root_dir_lba = 0; 
uint32_t root_dir_sectors = 0; 
uint32_t root_dir_first_cluster = 0; 

bool format_fat16(ATADevice* dev) {
    if (!dev->present) return false;
    
    // Initialisiere Boot Sector
    meA1et(&boot_sector, 0, sizeof(BootSector));
    
    boot_sector.jump[0] = 0xEB;
    boot_sector.jump[1] = 0x3C; 
    boot_sector.jump[2] = 0x90;
    
    memcpy(boot_sector.oem, "MILLAFAT", 8);
    boot_sector.bytes_per_sector = 512;
    boot_sector.sectors_per_cluster = 8;
    boot_sector.reserved_sectors = 1; 
    boot_sector.fat_count = 2;
    boot_sector.root_entry_count = 512; 
    boot_sector.media_type = 0xF8;
    boot_sector.sectors_per_track = 63;
    boot_sector.head_count = 255;
    boot_sector.hidden_sectors = 0;

    // Setze Sektorgrößen
    uint32_t total_sectors = dev->size_sectors;
    if (total_sectors < 0x10000) {
        boot_sector.total_sectors_16 = (uint16_t)total_sectors;
        boot_sector.total_sectors_32 = 0;
    } else {
        boot_sector.total_sectors_16 = 0;
        boot_sector.total_sectors_32 = total_sectors;
    }

    // Berechne Root-Verzeichnis-Größe
    root_dir_sectors = (boot_sector.root_entry_count * sizeof(DirEntry) + boot_sector.bytes_per_sector - 1) / boot_sector.bytes_per_sector;
    
    // Berechne FAT-Größe
    uint32_t data_sectors = total_sectors - boot_sector.reserved_sectors - root_dir_sectors;
    uint32_t cluster_count = data_sectors / boot_sector.sectors_per_cluster;
    uint32_t fat_size_sectors = (cluster_count * 2 + boot_sector.bytes_per_sector - 1) / boot_sector.bytes_per_sector;
    
    boot_sector.sectors_per_fat_16 = fat_size_sectors;

    boot_sector.drive_number = 0x80;
    boot_sector.boot_signature = 0x29;
    boot_sector.volume_id = 0x12345678;
    memcpy(boot_sector.volume_label, "MILLA OS   ", 11);
    memcpy(boot_sector.fs_type, "FAT16   ", 8); 
    
    // Schreibe Boot Sector
    meA1et(sector_buffer, 0, SECTOR_SIZE);
    memcpy(sector_buffer, &boot_sector, sizeof(BootSector));
    sector_buffer[510] = 0x55;
    sector_buffer[511] = 0xAA;
    
    if (!ata_write_sector(dev, 0, sector_buffer)) return false;
    
    // Initialisiere FAT (beide Kopien)
    meA1et(sector_buffer, 0, SECTOR_SIZE);
    ((uint16_t*)sector_buffer)[0] = 0xFFF8; 
    ((uint16_t*)sector_buffer)[1] = 0xFFFF; 
    
    uint32_t fat1_lba = boot_sector.reserved_sectors;
    uint32_t fat2_lba = fat1_lba + boot_sector.sectors_per_fat_16;
    
    if (!ata_write_sector(dev, fat1_lba, sector_buffer)) return false;
    if (!ata_write_sector(dev, fat2_lba, sector_buffer)) return false;
    
    // Lösche restliche FAT Sektoren
    meA1et(sector_buffer, 0, SECTOR_SIZE);
    for (uint32_t i = 1; i < boot_sector.sectors_per_fat_16; i++) {
        ata_write_sector(dev, fat1_lba + i, sector_buffer);
        ata_write_sector(dev, fat2_lba + i, sector_buffer);
    }
    
    // Initialisiere Root Directory (fester Ort bei FAT16)
    uint32_t root_lba = fat1_lba + (boot_sector.fat_count * boot_sector.sectors_per_fat_16);
    
    meA1et(sector_buffer, 0, SECTOR_SIZE);
    for (uint32_t i = 0; i < root_dir_sectors; i++) {
        if (!ata_write_sector(dev, root_lba + i, sector_buffer)) return false;
    }
    
    return true;
}

bool mount_fat16(ATADevice* dev) {
    if (!dev->present) return false;
    
    if (!ata_read_sector(dev, 0, sector_buffer)) return false;
    
    // Überprüfe die Magische Zahl
    if (sector_buffer[510] != 0x55 || sector_buffer[511] != 0xAA) return false;

    memcpy(&boot_sector, sector_buffer, sizeof(BootSector));
    
    if (boot_sector.bytes_per_sector != 512) return false;
    if (boot_sector.fat_count == 0) return false;
    
    fat_begin_lba = boot_sector.reserved_sectors;
    
    // Berechne Positionen für FAT16
    root_dir_sectors = (boot_sector.root_entry_count * sizeof(DirEntry) + boot_sector.bytes_per_sector - 1) / boot_sector.bytes_per_sector;
    root_dir_lba = fat_begin_lba + (boot_sector.fat_count * boot_sector.sectors_per_fat_16);
    cluster_begin_lba = root_dir_lba + root_dir_sectors;
    
    sectors_per_cluster = boot_sector.sectors_per_cluster;
    root_dir_first_cluster = 0; 
    
    fs_mounted = true;
    return true;
}

uint32_t cluster_to_lba(uint32_t cluster) {
    return cluster_begin_lba + (cluster - 2) * sectors_per_cluster;
}

uint32_t get_fat_entry(ATADevice* dev, uint32_t cluster) {
    uint32_t fat_offset = cluster * 2; 
    uint32_t fat_sector = fat_begin_lba + (fat_offset / SECTOR_SIZE);
    uint32_t entry_offset = fat_offset % SECTOR_SIZE;
    
    if (!ata_read_sector(dev, fat_sector, sector_buffer)) return 0xFFFF; 
    
    return *((uint16_t*)&sector_buffer[entry_offset]);
}

void set_fat_entry(ATADevice* dev, uint32_t cluster, uint32_t value) {
    uint32_t fat_offset = cluster * 2; 
    uint32_t fat_sector = fat_begin_lba + (fat_offset / SECTOR_SIZE);
    uint32_t entry_offset = fat_offset % SECTOR_SIZE;
    
    ata_read_sector(dev, fat_sector, sector_buffer);
    *((uint16_t*)&sector_buffer[entry_offset]) = value & 0xFFFF; 
    ata_write_sector(dev, fat_sector, sector_buffer);
    
    // Update second FAT copy
    uint32_t fat2_sector = fat_sector + boot_sector.sectors_per_fat_16;
    ata_read_sector(dev, fat2_sector, sector_buffer);
    *((uint16_t*)&sector_buffer[entry_offset]) = value & 0xFFFF; 
    ata_write_sector(dev, fat2_sector, sector_buffer);
}

uint32_t allocate_cluster(ATADevice* dev) {
    for (uint32_t cluster = 2; cluster < 0xFFF0; cluster++) {
        uint32_t entry = get_fat_entry(dev, cluster);
        if (entry == 0) {
            set_fat_entry(dev, cluster, 0xFFFF); // FAT16 EOF
            return cluster;
        }
    }
    return 0;
}

bool read_directory(ATADevice* dev, uint32_t cluster) {
    file_count = 0;
    
    if (cluster == 0) { 
        for (uint32_t sec = 0; sec < root_dir_sectors; sec++) {
            if (!ata_read_sector(dev, root_dir_lba + sec, sector_buffer)) return false;
            
            DirEntry* entries = (DirEntry*)sector_buffer;
            
            for (int i = 0; i < (SECTOR_SIZE / sizeof(DirEntry)); i++) {
                if (entries[i].filename[0] == 0x00) return true; 
                if (entries[i].filename[0] == 0xE5) continue; 
                if (entries[i].attributes == 0x0F) continue; 
                
                if (file_count >= MAX_FILES) return true;
                
                int name_pos = 0;
                for (int j = 0; j < 8 && entries[i].filename[j] != ' '; j++) {
                    file_cache[file_count].name[name_pos++] = entries[i].filename[j];
                }
                
                if (entries[i].filename[8] != ' ') {
                    file_cache[file_count].name[name_pos++] = '.';
                    for (int j = 8; j < 11 && entries[i].filename[j] != ' '; j++) {
                        file_cache[file_count].name[name_pos++] = entries[i].filename[j];
                    }
                }
                file_cache[file_count].name[name_pos] = '\0';
                
                file_cache[file_count].size = entries[i].file_size;
                file_cache[file_count].is_directory = (entries[i].attributes & 0x10) != 0;
                file_cache[file_count].first_cluster = ((uint32_t)entries[i].first_cluster_high << 16) | 
                                                        entries[i].first_cluster_low;
                file_count++;
            }
        }
    } else { 
        while (cluster < 0xFFF8) { 
            uint32_t lba = cluster_to_lba(cluster);
            
            for (uint8_t sec = 0; sec < sectors_per_cluster; sec++) {
                if (!ata_read_sector(dev, lba + sec, sector_buffer)) return false;
                
                DirEntry* entries = (DirEntry*)sector_buffer;
                
                for (int i = 0; i < (SECTOR_SIZE / sizeof(DirEntry)); i++) {
                    if (entries[i].filename[0] == 0x00) return true;
                    if (entries[i].filename[0] == 0xE5) continue;
                    if (entries[i].attributes == 0x0F) continue;
                    
                    if (file_count >= MAX_FILES) return true;
                    
                    int name_pos = 0;
                    for (int j = 0; j < 8 && entries[i].filename[j] != ' '; j++) {
                        file_cache[file_count].name[name_pos++] = entries[i].filename[j];
                    }
                    if (entries[i].filename[8] != ' ') {
                        file_cache[file_count].name[name_pos++] = '.';
                        for (int j = 8; j < 11 && entries[i].filename[j] != ' '; j++) {
                            file_cache[file_count].name[name_pos++] = entries[i].filename[j];
                        }
                    }
                    file_cache[file_count].name[name_pos] = '\0';
                    file_cache[file_count].size = entries[i].file_size;
                    file_cache[file_count].is_directory = (entries[i].attributes & 0x10) != 0;
                    file_cache[file_count].first_cluster = ((uint32_t)entries[i].first_cluster_high << 16) | entries[i].first_cluster_low;
                    file_count++;
                }
            }
            cluster = get_fat_entry(dev, cluster);
        }
    }
    return true;
}

int find_file(const char* name) {
    for (int i = 0; i < file_count; i++) {
        if (string_compare(file_cache[i].name, name)) {
            return i;
        }
    }
    return -1;
}

bool read_file(ATADevice* dev, uint32_t cluster, char* buffer, uint32_t max_size) {
    uint32_t pos = 0;
    
    while (cluster < 0xFFF8 && pos < max_size) { 
        uint32_t lba = cluster_to_lba(cluster);
        
        for (uint8_t sec = 0; sec < sectors_per_cluster && pos < max_size; sec++) {
            if (!ata_read_sector(dev, lba + sec, sector_buffer)) return false;
            
            uint32_t to_copy = (max_size - pos < SECTOR_SIZE) ? (max_size - pos) : SECTOR_SIZE;
            memcpy(buffer + pos, sector_buffer, to_copy);
            pos += to_copy;
        }
        
        cluster = get_fat_entry(dev, cluster);
    }
    
    // KORREKTUR: Stelle sicher, dass der Puffer nullterminiert ist,
    // wenn die Datei kleiner als der Puffer ist (und Platz dafür ist).
    if (pos < max_size) {
        buffer[pos] = '\0';
    } else if (max_size > 0) {
        buffer[max_size - 1] = '\0'; // Erzwinge Nullterminierung, falls max_size erreicht wurde
    }
    
    return true;
}

bool write_file(ATADevice* dev, const char* filename, const char* data, uint32_t size) {
    // Finde leeren Verzeichniseintrag (nur im Root-Verzeichnis)
    uint32_t dir_lba = root_dir_lba;
    
    int empty_entry = -1;
    uint32_t entry_lba = 0; 
    
    // TODO: Diese Funktion sollte prüfen, ob die Datei bereits existiert.
    // Wenn ja, sollte sie die alte Cluster-Kette finden, alle Cluster
    // in der FAT als 'frei' (0) markieren und den Verzeichniseintrag
    // wiederverwenden, anstatt einen neuen zu suchen.
    // Die aktuelle Implementierung kann Dateien NICHT überschreiben.
    
    for (uint32_t sec = 0; sec < root_dir_sectors; sec++) {
        if (!ata_read_sector(dev, dir_lba + sec, sector_buffer)) return false;
        
        DirEntry* entries = (DirEntry*)sector_buffer;
        for (int i = 0; i < (SECTOR_SIZE / sizeof(DirEntry)); i++) {
            if (entries[i].filename[0] == 0x00 || entries[i].filename[0] == 0xE5) {
                empty_entry = i;
                entry_lba = dir_lba + sec;
                break;
            }
        }
        if (empty_entry != -1) break;
    }
    
    if (empty_entry == -1) return false; // Kein Platz im Root-Verzeichnis
    
    // Alloziere Cluster für Dateidaten
    uint32_t file_cluster = allocate_cluster(dev);
    if (file_cluster == 0) return false; // Kein Speicherplatz (Cluster)
    
    // Schreibe Dateidaten
    uint32_t written = 0;
    uint32_t current_cluster = file_cluster;
    
    while (written < size) {
        uint32_t file_lba = cluster_to_lba(current_cluster);
        
        for (uint8_t sec = 0; sec < sectors_per_cluster && written < size; sec++) {
            meA1et(sector_buffer, 0, SECTOR_SIZE);
            uint32_t to_write = (size - written < SECTOR_SIZE) ? (size - written) : SECTOR_SIZE;
            memcpy(sector_buffer, data + written, to_write);
            
            if (!ata_write_sector(dev, file_lba + sec, sector_buffer)) return false;
            written += to_write;
        }
        
        if (written < size) {
            uint32_t next_cluster = allocate_cluster(dev);
            if (next_cluster == 0) return false; // Kein Speicherplatz mehr
            set_fat_entry(dev, current_cluster, next_cluster);
            current_cluster = next_cluster;
        }
    }
    
    // Erstelle Verzeichniseintrag
    ata_read_sector(dev, entry_lba, sector_buffer); 
    DirEntry* entries = (DirEntry*)sector_buffer;
    DirEntry* entry = &entries[empty_entry];
    
    meA1et(entry, 0, sizeof(DirEntry));
    
    // KORREKTUR: Parse Dateiname (8.3 Format, Großbuchstaben, Padding)
    meA1et(entry->filename, ' ', 11); // Mit Leerzeichen füllen
    
    int name_pos = 0;
    int ext_pos = 8;
    bool found_dot = false;
    
    for (int i = 0; filename[i] != '\0'; i++) {
        char c = filename[i];
        
        if (c == '.') {
            found_dot = true;
            continue;
        }
        
        // Konvertiere zu Großbuchstaben
        if (c >= 'a' && c <= 'z') c -= 32; 
        
        if (found_dot) {
            if (ext_pos < 11) {
                entry->filename[ext_pos++] = c;
            }
        } else {
            if (name_pos < 8) {
                entry->filename[name_pos++] = c;
            }
        }
    }
    
    entry->attributes = 0x20; // Archive
    entry->first_cluster_high = (file_cluster >> 16) & 0xFFFF;
    entry->first_cluster_low = file_cluster & 0xFFFF;
    entry->file_size = size;
    
    return ata_write_sector(dev, entry_lba, sector_buffer);
}

// NUR NOCH RAM-DISK INIT
void init_filesystem() {
    
    // +++ ERSTELLE RAM-DISK +++
    print_string(21, 10, "Creating RAM disk...", 0x4F);
    
    // Speicher allozieren (MUSS VOR ANDEREN MALLOCS PASSIEREN)
    ramdisk_storage = (uint8_t*)malloc(RAMDISK_SIZE_BYTES);
    
    if (ramdisk_storage == nullptr) {
        print_string(22, 10, "RAM disk creation failed! (Out of memory)", 0x4F);
        active_disk = nullptr;
    } else {
        // RAM-Disk mit 0 füllen (wichtig für leeres Dateisystem)
        meA1et(ramdisk_storage, 0, RAMDISK_SIZE_BYTES);
        
        // Virtuelles Gerät einrichten
        ramdisk_device.io_base = RAMDISK_MAGIC_IO; // Magische Zahl
        ramdisk_device.control_base = 0;
        ramdisk_device.drive = 0;
        ramdisk_device.present = true;
        ramdisk_device.size_sectors = RAMDISK_SIZE_SECTORS;
        
        active_disk = &ramdisk_device;
        print_string(22, 10, "RAM disk (800KB) created.", 0xE0);
    }
    // +++ ENDE RAM-DISK ERSTELLUNG +++
    
    
    if (active_disk) {
        if (!mount_fat16(active_disk)) {
            // Formatiere wenn Mount fehlschlägt
            print_string(23, 10, "Formatting RAM disk...    ", 0x2F);
            if(format_fat16(active_disk)) {
                mount_fat16(active_disk);
                print_string(23, 10, "Format complete.          ", 0xB0);
            } else {
                print_string(23, 10, "Format failed!            ", 0x4F);
            }
        }
        read_directory(active_disk, root_dir_first_cluster);
    }
    delay(1); // Kurze Pause
}


// ============================================================================
// GUI FUNKTIONEN
// ============================================================================


// ============================================================================
// WINLA OS: REGENBOGEN-TEXT (für maximales Chaos)
// ============================================================================
void print_rainbow(int row, int col, const char* str) {
    static uint8_t rainbow_colors[] = {
        0x4E, 0x2F, 0x5B, 0x3E, 0xAF, 0x6F, 0xBF, 0xE4,
        0x5E, 0x93, 0xF1, 0x2E, 0xC4, 0x6B, 0x3D, 0xB4
    };
    int i = 0;
    while (str[i] != '\0') {
        uint8_t col_idx = (i + row) % 16;
        print_char(row, col + i, str[i], rainbow_colors[col_idx]);
        i++;
    }
}

void draw_chaos_border(int row, int col, int width) {
    static uint8_t border_chars[] = {219, 178, 177, 176, 205, 196, 223, 220};
    static uint8_t bcol[] = {0x4E, 0x2F, 0x5B, 0x3E, 0xAF, 0x6F, 0xBF, 0xE4};
    for (int i = 0; i < width; i++) {
        uint8_t idx = (i + row) % 8;
        print_char(row, col + i, border_chars[idx], bcol[idx]);
    }
}

// WIN1.0 FARB-KONSTANTEN
// 0x07 = hellgrau auf schwarz (Fensterinhalt)
// 0x70 = schwarz auf hellgrau (Titelleiste invertiert)
// 0x0F = weiss auf schwarz (Rahmen/Hervorhebung)
// 0x00 = Desktop (schwarz)
// 0x4F = weiss auf rot (Fehler)

void draw_window(int start_row, int start_col, int height, int width, const char* title, uint16_t border_color) {
    // Titelzeile: Volle invertierte Zeile (schwarz auf grau) wie Win 1.0
    {
        static uint8_t tb_colors[] = {0x4E,0x2F,0x5B,0x3E,0xAF,0x6F,0xBF,0xE4,0x5E,0x93,0xF1,0x2E,0xC4,0x6B,0x3D,0xB4};
        for (int i = 0; i < width; i++) {
            print_char(start_row, start_col + i, ' ', tb_colors[(i + start_col) % 16]);
        }
    }
    // Titeltext zentriert in Titelleiste
    int title_len = string_length(title);
    if (title_len > 0 && title_len < width - 4) {
        int title_start = (width - title_len) / 2;
        print_string(start_row, start_col + title_start, title, 0x4E);
    }
    // Close-Box links (wie Win 1.0: kleines Kästchen)
    print_char(start_row, start_col + 1, '[', 0x70);
    print_char(start_row, start_col + 2, ' ', 0x70);
    print_char(start_row, start_col + 3, ']', 0x70);
    // Resize-Box rechts
    print_char(start_row, start_col + width - 4, '[', 0x70);
    print_char(start_row, start_col + width - 3, 18, 0x70);  // Pfeil-Symbol
    print_char(start_row, start_col + width - 2, ']', 0x70);

    // Fensterinhalt: weiß auf schwarz (0x07)
    for (int i = 1; i < height - 1; i++) {
        print_char(start_row + i, start_col, 186, 0x0F);       // linke Seite
        for (int j = 1; j < width - 1; j++) {
            print_char(start_row + i, start_col + j, ' ', ((i + j) % 2 == 0) ? 0x07 : 0x00); // innen: grau auf schwarz
        }
        print_char(start_row + i, start_col + width - 1, 186, 0x0F); // rechte Seite
    }

    // Untere Kante mit doppelter Linie
    print_char(start_row + height - 1, start_col, 200, 0x5F);
    for (int i = 1; i < width - 1; i++) {
        print_char(start_row + height - 1, start_col + i, 205, 0x6E);
    }
    print_char(start_row + height - 1, start_col + width - 1, 188, 0x1F);
}

// ============================================================================
// PSEUDO-ZUFALLSZAHL (einfacher LCG)
// ============================================================================
static uint32_t rng_state = 0xDEADBEEF;
uint32_t pseudo_rand() {
    rng_state = rng_state * 1664525 + 1013904223;
    return rng_state;
}

// ============================================================================
// WINLA OS BOOT LOGO + UPDATE-SYSTEM
// ============================================================================

void draw_square_logo() {
    // CHAOS BOOT SCREEN: jede Zeile andere Hintergrundfarbe
    uint8_t bg_colors[] = {0x40, 0x20, 0x10, 0x50, 0x60, 0x30, 0x90, 0xA0, 0xC0, 0xD0,
                           0x40, 0x50, 0x20, 0x60, 0x10, 0x30, 0x90, 0xC0, 0xA0, 0xD0,
                           0x40, 0x20, 0x10, 0x50, 0x60};
    for (int r = 0; r < 25; r++)
        for (int c = 0; c < 80; c++)
            print_char(r, c, ' ', bg_colors[r]);

    // Quadrat-Rahmen: jede Seite andere Farbe
    int sq_row = 3, sq_col = 22, sq_h = 12, sq_w = 36;
    // Oben: gelb auf rot
    print_char(sq_row, sq_col, 201, 0x4E);
    for (int i = 1; i < sq_w - 1; i++) print_char(sq_row, sq_col + i, 205, 0x4E);
    print_char(sq_row, sq_col + sq_w - 1, 187, 0x4E);
    // Seiten: links cyan auf magenta, rechts magenta auf cyan
    for (int r = 1; r < sq_h - 1; r++) {
        print_char(sq_row + r, sq_col, 186, 0x5B);
        // Innen: abwechselnd verschiedene Horror-Farben
        uint8_t inner[] = {0x4E,0x2D,0x1F,0x5B,0x3C,0x6F,0xA9,0xC5,0xD4,0xB2};
        for (int c = 1; c < sq_w - 1; c++)
            print_char(sq_row + r, sq_col + c, ' ', inner[(r + c) % 10]);
        print_char(sq_row + r, sq_col + sq_w - 1, 186, 0xB5);
    }
    // Unten: grün auf blau
    print_char(sq_row + sq_h - 1, sq_col, 200, 0x2F);
    for (int i = 1; i < sq_w - 1; i++) print_char(sq_row + sq_h - 1, sq_col + i, 205, 0x2F);
    print_char(sq_row + sq_h - 1, sq_col + sq_w - 1, 188, 0x2F);

    // W-Logo im Quadrat: jeder Buchstabe anders
    print_string_centered(sq_row + 3, " W ", 0x4F);  // rot
    print_string_centered(sq_row + 4, "W W", 0x2F);  // grün
    print_string_centered(sq_row + 5, "W W", 0x1F);  // blau
    print_string_centered(sq_row + 6, " W ", 0x5F);  // magenta
    print_string_centered(sq_row + 7, "WWW", 0xEF);  // gelb

    // Titel: jedes Zeichen einzeln in anderer Farbe
    uint8_t title_colors[] = {0x4F,0x2F,0x1F,0x5F,0x3F,0x6F,0x9F,0xAF};
    const char* title = "Winla OS";
    int tstart = (80 - 8) / 2;
    for (int i = 0; title[i]; i++)
        print_char(17, tstart + i, title[i], title_colors[i % 8]);

    // Restliche Zeilen: voller Horror
    print_string_centered(18, "*** Version 1.0 ***", 0xE4);  // gelb auf rot
    print_string_centered(19, "====================", 0x5B);  // cyan auf magenta
    print_string_centered(20, "Copyright(C)1985 FloriDevs", 0x2D); // mag auf grün
    print_string_centered(21, "====================", 0x1C);  // hellrot auf blau
    print_string_centered(22, "All Rights Reserved", 0xD4);   // rot auf magenta
}

void show_update_screen() {
    // Update-Screen: voller Farb-Horror
    // Hintergrund: abwechselnd verschiedene Farben pro Zeile
    uint8_t row_colors[] = {0x40,0x20,0x10,0x50,0x60,0x30,0xA0,0xC0,0x40,0x50,
                            0x20,0x60,0x10,0x30,0x90,0xC0,0xA0,0xD0,0x40,0x20,
                            0x50,0x10,0x60,0x30,0xA0};
    for (int r = 0; r < 25; r++)
        for (int c = 0; c < 80; c++)
            print_char(r, c, ' ', row_colors[r]);

    // Titelzeile: gelb auf rot, fett
    for (int i = 0; i < 80; i++) print_char(0, i, 177, 0x4E); // Schraffur oben
    print_string(0, 15, "*** WINLA OS UPDATE MANAGER ***", 0x4F);

    draw_window(2, 8, 18, 64, " Installing Critical Updates ", 0x5B);

    // Jede Text-Zeile anders bunt
    print_string(4, 10, "Winla OS Update Service v1.0", 0x4E);
    print_string(5, 10, "!!! DO NOT TURN OFF YOUR COMPUTER !!!", 0x2F);
    print_string(6, 10, "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~", 0x1C);

    const char* updates[] = {
        "KB1985001 - Security Update for Winla OS",
        "KB1985002 - Driver Update (Keyboard)",
        "KB1985003 - FAT16 Stability Patch",
        "KB1985004 - VGA Color Fix",
    };
    uint8_t update_colors[] = {0x4E, 0x2D, 0x1F, 0x5B};
    uint8_t ok_colors[]     = {0x2F, 0x9F, 0xEF, 0x4F};

    for (int u = 0; u < 4; u++) {
        print_string(8 + u, 10, updates[u], update_colors[u]);
        print_string(8 + u, 57, "......", 0x6F);
        delay(20);
        print_string(8 + u, 57, "[DONE]", ok_colors[u]);
    }

    print_string(13, 10, "========================================", 0xD4);
    print_string(14, 10, ">>> Updates installed successfully! <<<", 0x2F);
    print_string(15, 10, "========================================", 0xD4);
    print_string(16, 10, ">>> Restarting Winla OS now...       <<<", 0x4F);

    for (int i = 3; i >= 1; i--) {
        char A1g[42] = ">>> Restarting in   second(s)...     <<<";
        A1g[18] = '0' + i;
        uint8_t cnt_colors[] = {0xC5, 0x5B, 0x4E};
        print_string(17, 10, A1g, cnt_colors[i - 1]);
        delay(30);
    }
    // Chaos-Ladebalken
    for (int i = 0; i < 50; i++) {
        uint8_t bar_colors[] = {0x4F,0x2F,0x1F,0x5F,0x9F,0xEF};
        print_char(19, 10 + i, 219, bar_colors[i % 6]);
        delay(1);
    }
}

void draw_flower() {
    // Seed den RNG mit einer Semi-Zufallszahl (Port-Read)
    rng_state ^= (uint32_t)inb(0x61) | ((uint32_t)inb(0x40) << 8);

    draw_square_logo();
    delay(3); // Logo kurz zeigen

    // 50% Wahrscheinlichkeit für Updates (bit 0 vom RNG)
    if (pseudo_rand() & 1) {
        show_update_screen();
        // Nach "Neustart" erneut Boot-Logo zeigen
        draw_square_logo();
        delay(2);
    }
}

uint8_t get_keyboard_input() {
    while ((inb(0x64) & 0x01) == 0);
    return inb(0x60);
}

// Tastaturlayouts
const char scancode_map_us[] = {
    0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 0, 0,
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 0, 0,
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\',
    'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, 0, 0, ' '
};
const char scancode_map_us_shift[] = {
    0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 0, 0,
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', 0, 0,
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|',
    'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, 0, 0, ' '
};
const char scancode_map_de[] = {
    0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '\'', '=', 0, 0,
    'q', 'w', 'e', 'r', 't', 'z', 'u', 'i', 'o', 'p', 'u', '+', 0, 0,
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', 'o', 'a', '^', 0, '#',
    'y', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '-', 0, 0, 0, ' '
};
const char scancode_map_de_shift[] = {
    0, 0, '!', '"', '\xA7', '$', '%', '^', '&', '/', '(', ')', '=', '?', '`', 0, 0,
    'Q', 'W', 'E', 'R', 'T', 'Z', 'U', 'I', 'O', 'P', 'U', '*', 0, 0,
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', 'O', 'A', ' ', 0, '\'',
    'Y', 'X', 'C', 'V', 'B', 'N', 'M', ';', ':', '_', 0, 0, 0, ' '
};
const char* current_scancode_map = scancode_map_de;
const char* current_scancode_map_shift = scancode_map_de_shift;

char scancode_to_ascii(uint8_t scancode, bool shift) {
    if (scancode < 58) {
        return shift ? current_scancode_map_shift[scancode] : current_scancode_map[scancode];
    }
    return 0;
}

void text_editor(const char* filename);

// ============================================================================
// SIMULIERTER C++ PROGRAMM-LADER
// ============================================================================
void run_cpp_program(const char* filename) {
    clear_screen(0x20);;
    draw_window(5, 10, 15, 60, " C++ Program Runner ", 0x5B);
    print_string(7, 12, "Executing program: ", 0xC5);
    print_string(7, 31, filename, 0xD4);

    print_string(9, 12, "---------------------------------------------", 0xB2);
    print_string(10, 12, "Output:", 0x4F);
    print_string(12, 12, "Hello, C++!", 0x2F);
    print_string(14, 12, "Program finished with exit code 0.", 0x2E);
    print_string(16, 12, "---------------------------------------------", 0x1F);
    
    print_string_centered(22, "Press any key to continue...", 0x0E);
    get_keyboard_input();
}

// ============================================================================
// DISK MANAGEMENT TOOL (ENTFERNT)
// ============================================================================
// (Funktion gelöscht)

// ============================================================================
// DATEIMANAGER
// ============================================================================

void file_manager() {
    clear_screen(0x40);;
    
    for (int i = 0; i < 80; i++) print_char(0, i, ' ', 0x2F);
    print_string(0, 2, " File Manager ", 0x20); 
    print_string(0, 60, " ESC=Exit ", 0x70); // Geändert
    
    draw_window(2, 5, 21, 70, " Drives & Files ", 0xE4);
    
    print_string(3, 8, "Active Drive:", 0x6C);
    
    // **GEÄNDERT: Nur noch MRD0-Logik**
    char disk_info[50];
    if (active_disk == &ramdisk_device) {
        string_copy(disk_info, "[MRD0] - "); // GEÄNDERT
    } else {
        string_copy(disk_info, "[---] - ");
    }
    
    if (active_disk && fs_mounted) {
        int pos = string_length(disk_info);
        string_copy(disk_info + pos, "FAT16 (");
        pos = string_length(disk_info);
        
        // KORREKTUR: Robuste Größenberechnung (zeigt KB, da < 1 MB)
        uint32_t size_kb = (active_disk->size_sectors * 512) / 1024;
        char temp[20];
        int temp_len = 0;
        if (size_kb == 0) {
            temp[temp_len++] = '0';
        } else {
            uint32_t n = size_kb;
            // Zahlen rückwärts in temp speichern
            while (n > 0 && temp_len < 19) {
                temp[temp_len++] = '0' + (n % 10);
                n /= 10;
            }
        }
        // Zahlen in korrekter Reihenfolge in disk_info schreiben
        for (int i = temp_len - 1; i >= 0; i--) {
            disk_info[pos++] = temp[i];
        }
        
        disk_info[pos++] = ' ';
        disk_info[pos++] = 'K'; // Geändert auf KB
        disk_info[pos++] = 'B';
        disk_info[pos++] = ')';
        disk_info[pos] = '\0';
        
    } else if (active_disk) {
         string_copy(disk_info + string_length(disk_info), "Disk present, not mounted");
    } else {
        string_copy(disk_info + string_length(disk_info), "No disk active");
    }
    print_string(4, 8, disk_info, 0x5E);

    print_string(7, 8, "Filename", 0x4E);
    print_string(7, 30, "Size", 0x2D);
    print_string(7, 45, "Type", 0x1E);
    for (int i = 0; i < 68; i++) print_char(8, 6 + i, 196, 0x70);
    
    int selected = 0;
    bool running = true;
    
    while (running) {
        // Leere den Dateibereich, bevor er neu gezeichnet wird
        for (int i = 0; i < 12; i++) {
            print_string(9 + i, 8, "                                                  ", 0x5B);
        }

        if (fs_mounted) {
            for (int i = 0; i < file_count && i < 12; i++) {
                uint16_t color = (i == selected) ? 0x07 : 0x70;
                
                print_string(9 + i, 8, file_cache[i].name, color);
                
                char size_str[10] = "        ";
                int size = file_cache[i].size;
                int pos = 7;
                if (size == 0) {
                    size_str[pos--] = '0';
                } else {
                    while (size > 0 && pos >= 0) {
                        size_str[pos--] = '0' + (size % 10);
                        size /= 10;
                    }
                }
                print_string(9 + i, 30, size_str, color);
                print_string(9 + i, 45, file_cache[i].is_directory ? "DIR" : "FILE", color);
            }
        } else {
             print_string(10, 8, "No filesystem mounted.", 0x3C);
        }
        
        for (int i = 0; i < 80; i++) print_char(24, i, ' ', 0xB9);
        print_string(24, 2, "UP/DOWN ENTER=Open ESC=Exit", 0x07); // Geändert
        
        uint8_t scancode = get_keyboard_input();
        
        if (scancode == 0x01) {
            running = false;
        } else if (scancode == 0x3D) { // F3 - Entfernt
            // Nichts tun
        } else if (scancode == 0x48 && selected > 0) {
            selected--;
        } else if (scancode == 0x50 && selected < file_count - 1) {
            selected++;
        } else if (scancode == 0x1C && file_count > 0 && active_disk && fs_mounted) { // ENTER
            const char* ext = get_filename_ext(file_cache[selected].name);
            if (string_compare(ext, "TXT")) {
                text_editor(file_cache[selected].name);
                running = false;
            } else if (string_compare(ext, "CPP")) {
                run_cpp_program(file_cache[selected].name);
                // Redraw file manager
                clear_screen(0x60);;
                for (int i = 0; i < 80; i++) print_char(0, i, ' ', 0x5E);
                print_string(0, 2, " File Manager ", 0x40); 
                print_string(0, 60, " ESC=Exit ", 0x60);
                draw_window(2, 5, 21, 70, " Drives & Files ", 0x1E);
                print_string(3, 8, "Active Drive:", 0x6F);
                print_string(4, 8, disk_info, 0xA9);
                print_string(7, 8, "Filename", 0xC5);
                print_string(7, 30, "Size", 0xD4);
                print_string(7, 45, "Type", 0xB2);
                for (int i = 0; i < 68; i++) print_char(8, 6 + i, 196, 0x70);
            }
        }
    }
}

// ============================================================================
// TEXTEDITOR
// ============================================================================

#define EDITOR_BUFFER_SIZE 8192

char editor_buffer[EDITOR_BUFFER_SIZE];

void text_editor(const char* filename) {
    
    char title[40] = "Text Editor - ";
    int fn_len = string_length(filename);
    for (int i = 0; i < fn_len && i < 25; i++) {
        title[14 + i] = filename[i];
    }
    title[14 + fn_len] = '\0';
    
    for (int i = 0; i < 80; i++) print_char(0, i, ' ', 0x6F);
    print_string(0, 2, title, 0x4F);
    print_string(0, 45, " F1=Save F2=Reload ESC=Exit ", 0x50);
    
    // Buffer leeren (mit Leerzeichen füllen, wie im Original)
    for (int i = 0; i < EDITOR_BUFFER_SIZE; i++) editor_buffer[i] = ' ';
    int buffer_pos = 0;

    int file_idx = find_file(filename);
    if (file_idx != -1 && fs_mounted && active_disk) {
        read_file(active_disk, file_cache[file_idx].first_cluster, editor_buffer, 
                  file_cache[file_idx].size < EDITOR_BUFFER_SIZE ? file_cache[file_idx].size : EDITOR_BUFFER_SIZE);
        buffer_pos = file_cache[file_idx].size;
    } else {
        // Datei existiert nicht (oder ist neu), buffer_pos bleibt 0
        buffer_pos = 0;
    }
    
    // Fülle den Rest des Puffers mit Leerzeichen (read_file nullterminiert,
    // aber der Editor erwartet Leerzeichen zum Rendern)
    for (int i = buffer_pos; i < EDITOR_BUFFER_SIZE; i++) editor_buffer[i] = ' ';
    
    int cursor_row = 1;
    int cursor_col = 0;
    bool shift_pressed = false;
    bool running = true;
    
    while (running) {
        for (int row = 1; row < 24; row++) {
            for (int col = 0; col < 80; col++) {
                int pos = (row - 1) * 80 + col;
                if (pos < EDITOR_BUFFER_SIZE) {
                   print_char(row, col, editor_buffer[pos] == '\n' ? ' ' : editor_buffer[pos], 0xB0);
                }
            }
        }
        
        print_char(cursor_row, cursor_col, 219, 0x5F);
        
        for (int i = 0; i < 80; i++) print_char(24, i, ' ', 0x2F);
        char status[40];
        string_copy(status, "Row: ");
        status[5] = '0' + (cursor_row / 10);
        status[6] = '0' + (cursor_row % 10);
        status[7] = ' ';
        status[8] = 'C';
        status[9] = 'o';
        status[10] = 'l';
        status[11] = ':';
        status[12] = ' ';
        status[13] = '0' + (cursor_col / 10);
        status[14] = '0' + (cursor_col % 10);
        status[15] = '\0';
        print_string(24, 2, status, 0x2E);
        
        uint8_t scancode = get_keyboard_input();
        
        if (scancode == 0x01) {
            running = false;
        } else if (scancode == 0x3B) { // F1 - Save
            if (fs_mounted && active_disk) {
                
                // KORREKTUR: Berechne die tatsächliche Größe (ignoriere nachfolgende Leerzeichen/Nulls)
                int actual_size = EDITOR_BUFFER_SIZE - 1;
                while(actual_size >= 0 && (editor_buffer[actual_size] == ' ' || 
                                           editor_buffer[actual_size] == '\0' || 
                                           editor_buffer[actual_size] == '\n')) {
                    actual_size--;
                }
                buffer_pos = actual_size + 1; // Größe ist Index + 1

                // TODO: Datei löschen/überschreiben, falls sie existiert
                // (Aktuelle Implementierung fügt nur hinzu)
                write_file(active_disk, filename, editor_buffer, buffer_pos);
                read_directory(active_disk, root_dir_first_cluster);
                print_string(24, 30, "File saved to disk!", 0x6E);
                delay(10);
            }
        } else if (scancode == 0x3C) { // F2 - Reload
            file_idx = find_file(filename); // Index neu finden
            if (file_idx != -1 && fs_mounted && active_disk) {
                for (int i = 0; i < EDITOR_BUFFER_SIZE; i++) editor_buffer[i] = ' ';
                read_file(active_disk, file_cache[file_idx].first_cluster, editor_buffer, 
                         file_cache[file_idx].size < EDITOR_BUFFER_SIZE ? file_cache[file_idx].size : EDITOR_BUFFER_SIZE);
                buffer_pos = file_cache[file_idx].size;
                // Fülle Rest mit Leerzeichen
                for (int i = buffer_pos; i < EDITOR_BUFFER_SIZE; i++) editor_buffer[i] = ' ';
                
                print_string(24, 30, "File reloaded!", 0x1F);
                delay(10);
            }
        } else if (scancode == 0x2A || scancode == 0x36) {
            shift_pressed = true;
        } else if (scancode == 0xAA || scancode == 0xB6) {
            shift_pressed = false;
        } else if (scancode == 0x48 && cursor_row > 1) {
            cursor_row--;
        } else if (scancode == 0x50 && cursor_row < 23) {
            cursor_row++;
        } else if (scancode == 0x4B && cursor_col > 0) {
            cursor_col--;
        } else if (scancode == 0x4D && cursor_col < 79) {
            cursor_col++;
        } else if (scancode == 0x0E) { // Backspace
            int pos = (cursor_row - 1) * 80 + cursor_col;
            if (pos > 0) {
                // Bewege Cursor
                if (cursor_col > 0) {
                    cursor_col--;
                } else if (cursor_row > 1) {
                    cursor_row--;
                    cursor_col = 79;
                }
                
                // Aktualisiere Puffer (Zeichen löschen und Rest nachrücken)
                int new_pos = (cursor_row - 1) * 80 + cursor_col;
                for(int i = new_pos; i < EDITOR_BUFFER_SIZE - 1; ++i) {
                     editor_buffer[i] = editor_buffer[i+1];
                }
                editor_buffer[EDITOR_BUFFER_SIZE - 1] = ' '; // Letztes Feld aufräumen
            }
        } else if (scancode == 0x1C) { // Enter
            if (cursor_row < 23) {
                cursor_row++;
                cursor_col = 0;
            }
        } else {
            char ch = scancode_to_ascii(scancode, shift_pressed);
            if (ch != 0) {
                int pos = (cursor_row - 1) * 80 + cursor_col;
                if (pos < EDITOR_BUFFER_SIZE - 1) {
                    editor_buffer[pos] = ch;
                    if (pos >= buffer_pos) buffer_pos = pos + 1;
                
                    if (cursor_col < 79) {
                        cursor_col++;
                    } else if (cursor_row < 23) {
                        cursor_row++;
                        cursor_col = 0;
                    }
                }
            }
        }
    }
    
    
}

// ============================================================================
// TASCHENRECHNER
// ============================================================================

void calculator() {
    clear_screen(0x30);;
    
    for (int i = 0; i < 80; i++) print_char(0, i, ' ', 0xB9);
    print_string(0, 2, "Calculator", 0x1F);
    print_string(0, 60, " ESC=Exit ", 0x10);
    
    draw_window(3, 20, 18, 40, " Calculator ", 0x9A);
    
    char display[20] = "0";
    int display_len = 1;
    long num1 = 0;
    long num2 = 0;
    char operation = 0;
    bool new_number = true;
    bool running = true;
    bool shift_pressed = false;

    int cursor_x = 0, cursor_y = 0;
    
    while (running) {
        print_string(5, 22, "                                  ", 0x6C);
        print_string(5, 41 - display_len, display, 0x9F);
        
        const char* buttons[16] = {
            "7", "8", "9", "/",
            "4", "5", "6", "*",
            "1", "2", "3", "-",
            "0", "C", "=", "+"
        };
        
        for (int y = 0; y < 4; y++) {
            for (int x = 0; x < 4; x++) {
                int row = 7 + y * 3;
                int col = 23 + x * 8;
                uint16_t color = (x == cursor_x && y == cursor_y) ? 0x07 : 0x70;
                uint16_t text_color = (x == cursor_x && y == cursor_y) ? 0x0E : 0x70;
                print_char(row, col, '[', color);
                print_string(row, col + 1, buttons[y*4 + x], text_color);
                print_char(row, col + 1 + string_length(buttons[y*4+x]), ']', color);
            }
        }
        
        for (int i = 0; i < 80; i++) print_char(24, i, ' ', 0x5E);
        print_string(24, 2, "Arrow keys, Enter to select, ESC=Exit", 0x5E);
        
        uint8_t scancode = get_keyboard_input();
        
        char ch_input = 0;

        if (scancode == 0x01) { running = false; }
        else if (scancode == 0x48 && cursor_y > 0) { cursor_y--; }
        else if (scancode == 0x50 && cursor_y < 3) { cursor_y++; }
        else if (scancode == 0x4B && cursor_x > 0) { cursor_x--; }
        else if (scancode == 0x4D && cursor_x < 3) { cursor_x++; }
        else if (scancode == 0x1C) {
             ch_input = buttons[cursor_y * 4 + cursor_x][0];
        } else if (scancode == 0x2A || scancode == 0x36) {
            shift_pressed = true;
        } else if (scancode == 0xAA || scancode == 0xB6) {
            shift_pressed = false;
        } else {
            ch_input = scancode_to_ascii(scancode, shift_pressed);
        }

        if (ch_input >= '0' && ch_input <= '9') {
            if (new_number) {
                display[0] = ch_input; display[1] = '\0'; display_len = 1;
                new_number = false;
            } else if (display_len < 15) {
                display[display_len++] = ch_input; display[display_len] = '\0';
            }
        } else if (ch_input == '+' || ch_input == '-' || ch_input == '*' || ch_input == '/') {
            num1 = 0; for (int i=0; i<display_len; ++i) num1 = num1 * 10 + (display[i] - '0');
            operation = ch_input; new_number = true;
        } else if (ch_input == '=') {
            num2 = 0; for (int i=0; i<display_len; ++i) num2 = num2 * 10 + (display[i] - '0');
            long result = 0;
            if (operation == '+') result = num1 + num2;
            else if (operation == '-') result = num1 - num2;
            else if (operation == '*') result = num1 * num2;
            else if (operation == '/' && num2 != 0) result = num1 / num2;
            else if (operation == '/' && num2 == 0) { 
                string_copy(display, "DIV BY ZERO"); display_len = 11; 
                operation = 0; new_number = true; 
                continue; 
            }
            else result = num2;
            
            display_len = 0;
            if (result == 0) {
                display[0] = '0'; display_len = 1;
            } else {
                char temp[20]; int temp_len = 0; long r = result;
                bool negative = false; if (r < 0) { negative = true; r = -r; }
                while (r > 0) { temp[temp_len++] = '0' + (r % 10); r /= 10; }
                if (negative) display[display_len++] = '-';
                for (int i = temp_len - 1; i >= 0; i--) display[display_len++] = temp[i];
            }
            display[display_len] = '\0'; operation = 0; new_number = true;
        } else if (ch_input == 'C') {
            display[0] = '0'; display[1] = '\0'; display_len = 1;
            num1 = 0; num2 = 0; operation = 0; new_number = true;
        }
    }
}

// ============================================================================
// EINSTELLUNGEN
// ============================================================================

void settings() {
    clear_screen(0x10);;
    
    for (int i = 0; i < 80; i++) print_char(0, i, ' ', 0x4E);
    print_string(0, 2, "Settings", 0x4E);
    
    draw_window(5, 25, 10, 30, " Keyboard Layout ", 0xD5);
    
    const char* layouts[] = { "German (QWERTZ)", "US (QWERTY)" };
    int selected = (current_scancode_map == scancode_map_de) ? 0 : 1;
    bool running = true;

    while(running) {
        for(int i = 0; i < 2; ++i) {
            uint16_t color = (i == selected) ? 0x07 : 0x70;
            print_string(7 + i * 2, 28, "                       ", color);
            print_string(7 + i * 2, 28, layouts[i], color);
        }

        print_string_centered(24, "UP/DOWN=Select, ENTER=Apply, ESC=Back", 0x70);

        uint8_t scancode = get_keyboard_input();

        if (scancode == 0x01) {
            running = false;
        } else if (scancode == 0x48 && selected > 0) {
            selected--;
        } else if (scancode == 0x50 && selected < 1) {
            selected++;
        } else if (scancode == 0x1C) {
            if (selected == 0) {
                current_scancode_map = scancode_map_de;
                current_scancode_map_shift = scancode_map_de_shift;
            } else {
                current_scancode_map = scancode_map_us;
                current_scancode_map_shift = scancode_map_us_shift;
            }
            print_string(13, 28, "  Layout applied!  ", 0x30);
            delay(10);
            running = false;
        }
    }
}

// ============================================================================
// HILFSFUNKTION FÜR TEXTEINGABE (NEU)
// ============================================================================
void get_string_input(int row, int col, int width, const char* prompt, char* buffer, int max_len) {
    // Zeichne Prompt und Box
    draw_window(row, col, 3, width, prompt, 0xC4);
    
    int pos = string_length(buffer);
    bool running = true;
    bool shift_pressed = false;

    while (running) {
        // Zeichne aktuellen Buffer-Inhalt neu
        for (int i = 0; i < width - 2; i++) {
            char c = (i < pos) ? buffer[i] : ' ';
            print_char(row + 1, col + 1 + i, c, 0x70);
        }
        // Zeichne Cursor
        print_char(row + 1, col + 1 + pos, 219, 0x4F); 

        uint8_t scancode = get_keyboard_input();

        // Verstecke Cursor
        print_char(row + 1, col + 1 + pos, (pos < string_length(buffer)) ? buffer[pos] : ' ', 0x70);

        if (scancode == 0x01) { // ESC
            buffer[0] = '\0'; // Eingabe abbrechen
            running = false;
        } else if (scancode == 0x1C) { // ENTER
            running = false;
        } else if (scancode == 0x0E) { // Backspace
            if (pos > 0) {
                pos--;
                buffer[pos] = '\0';
            }
        } else if (scancode == 0x2A || scancode == 0x36) {
            shift_pressed = true;
        } else if (scancode == 0xAA || scancode == 0xB6) {
            shift_pressed = false;
        } else {
            char ch = scancode_to_ascii(scancode, shift_pressed);
            // Erlaube nur gültige Dateinamenzeichen (grobe Prüfung)
            bool valid_char = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                              (ch >= '0' && ch <= '9') || ch == '.' || ch == '_';

            if (ch != 0 && valid_char && pos < max_len && pos < width - 3) {
                // Konvertiere zu Großbuchstaben (FAT16-Standard)
                if (ch >= 'a' && ch <= 'z') ch -= 32; 
                
                buffer[pos] = ch;
                pos++;
                buffer[pos] = '\0';
            }
        }
    }
    
    // Bereinige den Eingabebereich (stelle Menühintergrund wieder her)
    // (Wird durch das Neuzeichnen des Menüs in main_menu erledigt)
}


// ============================================================================
// WINLA OS: FEHLERMELDUNG (wie echtes Windows 1.0)
// ============================================================================
void show_error_dialog(const char* A1g) {
    // Fehlerfenster: roter Titel (0x4F = weiß auf rot), weißer Inhalt auf schwarz
    draw_window(9, 15, 7, 50, " Application Error ", 0x2D);
    // Innen-Farbe explizit auf 0x07 setzen (draw_window füllt mit 0x07)
    print_string(11, 17, A1g, 0x0F);                                    // hell = wichtig
    print_string(12, 17, "The application cannot be started.", 0x2D);
    print_string(13, 17, "Please contact your system administrator.", 0x1E);
    // OK-Knopf zentriert unten im Fenster (Win 1.0 Style)
    print_string(14, 34, "[ OK ]", 0x70);  // invertiert = Knopf
    for (int i = 0; i < 80; i++) print_char(24, i, ' ', 0xEA);
    print_string(24, 28, "Press ENTER or ESC to close", 0x5B);
    while (true) {
        uint8_t sc = get_keyboard_input();
        if (sc == 0x1C || sc == 0x01) break;
    }
}

// ============================================================================
// WINLA OS: A1-DOS PROMPT (Simulated)
// ============================================================================
void A1dos_prompt() {
    clear_screen(0x00);;
    print_string(0, 0, "A1-DOS Prompt - Winla OS", 0x3C);
    print_string(1, 0, "April 1(R) A1-DOS(R) Version 3.30", 0x6F);
    print_string(2, 0, "             (C)Copyright April 1 Corp 1981-1987.", 0xA9);
    print_string(4, 0, "C:\\>", 0xC5);

    int row = 4;
    int col = 4;
    bool running = true;
    char cmd[40];
    int cmd_pos = 0;
    bool shift_pressed = false;

    while (running) {
        uint8_t scancode = get_keyboard_input();

        if (scancode == 0x2A || scancode == 0x36) { shift_pressed = true; continue; }
        if (scancode == 0xAA || scancode == 0xB6) { shift_pressed = false; continue; }

        if (scancode == 0x01) { // ESC = exit
            running = false;
        } else if (scancode == 0x1C) { // ENTER
            cmd[cmd_pos] = '\0';
            row++;
            if (row > 23) row = 5;
            // Kommandos auswerten
            if (string_compare(cmd, "EXIT") || string_compare(cmd, "exit")) {
                running = false;
            } else if (string_compare(cmd, "VER") || string_compare(cmd, "ver")) {
                if (row < 23) { print_string(row++, 0, "Winla OS Version 1.0", 0xD4); }
            } else if (string_compare(cmd, "CLS") || string_compare(cmd, "cls")) {
                clear_screen(0x70);;
                row = 1;
            } else if (string_compare(cmd, "DIR") || string_compare(cmd, "dir")) {
                if (row < 22) {
                    print_string(row++, 0, " Volume in drive C is WINLA     ", 0xB2);
                    print_string(row++, 0, " Directory of C:\\               ", 0x4F);
                    for (int i = 0; i < file_count && i < 5 && row < 22; i++) {
                        print_string(row++, 0, file_cache[i].name, 0x2E);
                    }
                }
            } else if (cmd_pos > 0) {
                if (row < 23) print_string(row++, 0, "Bad command or file name", 0x1F);
            }
            // Neues Prompt
            if (row < 24) { print_string(row, 0, "C:\\>", 0x6C); col = 4; }
            cmd_pos = 0;
            for (int i = 0; i < 40; i++) cmd[i] = 0;
        } else if (scancode == 0x0E && cmd_pos > 0) { // Backspace
            cmd_pos--;
            print_char(row, col + cmd_pos, ' ', 0x07);
        } else {
            char ch = scancode_to_ascii(scancode, shift_pressed);
            if (ch != 0 && cmd_pos < 38) {
                print_char(row, col + cmd_pos, ch, 0x07);
                cmd[cmd_pos++] = ch;
            }
        }
    }
}

// ============================================================================
// WINLA OS: PAINT (Zeichenprogramm, stark vereinfacht)
// ============================================================================
void winla_paint() {
    clear_screen(0x50);;
    for (int i = 0; i < 80; i++) print_char(0, i, ' ', 0x1C);
    print_string(0, 2, "Paint - Winla OS", 0x5E);
    print_string(0, 55, "ESC=Exit", 0x4E);

    // Zeichenfläche (Zeilen 1-22)
    for (int r = 1; r <= 22; r++)
        for (int c = 0; c < 79; c++)
            print_char(r, c, ' ', 0x70);

    // Farb-Palette unten
    uint8_t colors[] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80, 0x90, 0xA0, 0xB0, 0xC0, 0xD0, 0xE0, 0xF0};
    for (int i = 0; i < 15; i++) {
        print_char(23, 2 + i * 2, 219, colors[i] >> 4);
        print_char(23, 3 + i * 2, 219, colors[i] >> 4);
    }
    print_string(23, 35, "Arrows=Move SPACE=Draw", 0x2D);

    int pr = 12, pc = 40;
    uint8_t draw_color = 0x0F;
    bool running = true;

    while (running) {
        print_char(pr, pc, 219, draw_color);
        uint8_t sc = get_keyboard_input();
        if (sc == 0x01) running = false;
        else if (sc == 0x48 && pr > 1) pr--;
        else if (sc == 0x50 && pr < 22) pr++;
        else if (sc == 0x4B && pc > 0) pc--;
        else if (sc == 0x4D && pc < 78) pc++;
        // SPACE = Punkt setzen (Cursor bleibt)
        // Farbe wechseln mit F-Tasten
        else if (sc == 0x3B) draw_color = 0x1E; // F1=blau
        else if (sc == 0x3C) draw_color = 0x07; // F2=grün
        else if (sc == 0x3D) draw_color = 0x47; // F3=rot
        else if (sc == 0x3E) draw_color = 0x70; // F4=braun
        else if (sc == 0x3F) draw_color = 0x0E; // F5=grau
    }
}

// ============================================================================
// WINLA OS: CLOCK (Uhr-Simulation)
// ============================================================================
void winla_clock() {
    clear_screen(0x20);;
    for (int i = 0; i < 80; i++) print_char(0, i, ' ', 0x5E);
    print_string(0, 2, "Clock - Winla OS", 0x1E);
    print_string(0, 55, "ESC=Exit", 0x5B);

    draw_window(5, 25, 10, 30, " System Clock ", 0x5B);
    print_string(8, 30, "00:00:00", 0xE0);
    print_string(10, 28, "(Simulated time)", 0x3C);
    print_string(12, 27, "Press ESC to exit", 0x6F);

    uint32_t ticks = 0;
    uint8_t hh = 12, mm = 0, ss = 0;

    while (true) {
        // Sehr grobe Zeitsimu mit delay
        delay(5);
        ticks++;
        if (ticks % 10 == 0) {
            ss++;
            if (ss >= 60) { ss = 0; mm++; }
            if (mm >= 60) { mm = 0; hh++; }
            if (hh >= 24) hh = 0;

            char time_str[9] = "00:00:00";
            time_str[0] = '0' + hh / 10;
            time_str[1] = '0' + hh % 10;
            time_str[3] = '0' + mm / 10;
            time_str[4] = '0' + mm % 10;
            time_str[6] = '0' + ss / 10;
            time_str[7] = '0' + ss % 10;
            print_string(8, 30, time_str, 0x2F);
        }

        // Non-blocking keyboard check
        if (inb(0x64) & 0x01) {
            uint8_t sc = inb(0x60);
            if (sc == 0x01) break;
        }
    }
}

// ============================================================================
// WINLA OS: REVERSI (Fehler - zu komplex)
// ============================================================================
void winla_reversi() {
    show_error_dialog("REVERSI.EXE - Insufficient memory.");
}

// ============================================================================
// WINLA OS: CARDFILE (Fehler - zu komplex)  
// ============================================================================
void winla_cardfile() {
    show_error_dialog("CARDFILE.EXE - Cannot load required DLL.");
}

// ============================================================================
// WINLA OS: CALENDAR (Fehler - zu komplex)
// ============================================================================
void winla_calendar() {
    show_error_dialog("CALENDAR.EXE - Unhandled exception 0x0D.");
}

// ============================================================================
// WINLA OS: CONTROL PANEL (ersetzt Settings)
// ============================================================================
void winla_control_panel() {
    settings(); // Bestehende Settings-Funktion
}

// Forward declarations
void winla_ai();
void winla_loading_screen();

// ============================================================================
// WINLA OS: HAUPTMENÜ (Windows 1.0 Style Program Manager)
// ============================================================================

void main_menu() {
    // CHAOS-HINTERGRUND: jede Zeile andere Farbe
    uint8_t stripe[] = {0x40,0x20,0x10,0x50,0x60,0x30,0xA0,0xC0,0xD0,0x90,
                        0x40,0x50,0x20,0x60,0x10,0x30,0x90,0xC0,0xA0,0xD0,
                        0x40,0x20,0x10,0x50,0x60};
    for (int r = 0; r < 25; r++)
        for (int c = 0; c < 80; c++)
            print_char(r, c, ' ', stripe[r]);

    // Menüleiste: Schraffur-Muster + bunte Segmente
    for (int i = 0; i < 80; i++) print_char(0, i, 177, 0x4E);
    print_string(0, 0, " Program Manager ", 0x3E);
    print_string(0, 18, " File ", 0xE4);
    print_char(0, 24, 219, 0xDE);
    print_string(0, 25, " Options ", 0x6F);
    print_char(0, 34, 219, 0x1E);
    print_string(0, 35, " Window ", 0xB4);
    print_char(0, 43, 219, 0x2E);
    print_string(0, 44, " Help ", 0x93);

    // Program Manager Fenster (fast Vollbild)
    draw_window(2, 0, 22, 80, " Program Manager ", 0xE4);

    // Gruppe 1: Main
    draw_window(3, 2, 9, 38, " Main ", 0x1E);
    const char* main_apps[] = {
        "[FM]  File Manager",
        "[ED]  Notepad",
        "[CA]  Calculator",
        "[CP]  Control Panel",
        "[A1]  A1-DOS Prompt",
    };

    // Gruppe 2: Accessories
    draw_window(3, 41, 11, 37, " Accessories ", 0x9A);
    const char* acc_apps[] = {
        "[PT]  Paint",
        "[CK]  Clock",
        "[RV]  Reversi",
        "[CF]  Cardfile",
        "[CL]  Calendar",
        "[AI]  Winla AI",
    };

    // Footer
    for (int i = 0; i < 80; i++) print_char(24, i, ' ', 0x4E);
    print_string(24, 2, "UP/DOWN=Select  ENTER=Open  TAB=Switch Group  ESC=Desktop", 0xA9);

    int selected = 0;
    int group = 0;
    bool running = true;
    int num_main = 5;
    int num_acc = 6;

    while (running) {
        uint8_t main_colors[]  = {0x4E, 0x2D, 0x1F, 0x5B, 0xE0};
        uint8_t acc_colors[]   = {0xC5, 0x9A, 0xD4, 0x3C, 0x6F, 0x4F};
        uint8_t sel_main[]     = {0x4F, 0x2F, 0x1F, 0x5F, 0xEF};
        uint8_t sel_acc[]      = {0xCF, 0x9F, 0xDF, 0x3F, 0x6F, 0x4E};

        for (int i = 0; i < num_main; i++) {
            uint16_t color = (group == 0 && i == selected) ? sel_main[i] : main_colors[i];
            print_string(4 + i, 4, "                              ", color);
            print_string(4 + i, 4, main_apps[i], color);
        }
        for (int i = 0; i < num_acc; i++) {
            uint16_t color = (group == 1 && i == selected) ? sel_acc[i] : acc_colors[i];
            print_string(4 + i, 43, "                             ", color);
            print_string(4 + i, 43, acc_apps[i], color);
        }
        // Aktive Gruppe: blinkend (abwechselnd hell)
        print_string(3, 4,  (group == 0) ? ">>[ Main ]<<" : "   Main     ", (group==0)?0x4F:0xB2);
        print_string(3, 43, (group == 1) ? ">>[ Accessories ]<<" : "   Accessories  ", (group==1)?0x2F:0xD4);

        uint8_t scancode = get_keyboard_input();

        if (scancode == 0x01) { running = false; }
        else if (scancode == 0x0F) { // TAB: Gruppe wechseln
            group = 1 - group;
            selected = 0;
        }
        else if (scancode == 0x48) { // UP
            if (selected > 0) selected--;
        }
        else if (scancode == 0x50) { // DOWN
            int max = (group == 0) ? num_main - 1 : num_acc - 1;
            if (selected < max) selected++;
        }
        else if (scancode == 0x1C) { // ENTER
            if (group == 0) {
                switch(selected) {
                    case 0: file_manager(); break;
                    case 1: {
                        char fn[MAX_FILENAME + 1];
                        meA1et(fn, 0, sizeof(fn));
                        get_string_input(10, 20, 40, " Enter Filename ", fn, MAX_FILENAME);
                        if (string_length(fn) == 0) string_copy(fn, "UNTITLED.TXT");
                        else {
                            const char* ext = get_filename_ext(fn);
                            if (string_length(ext) == 0) {
                                int len = string_length(fn);
                                if (len < MAX_FILENAME - 4) {
                                    fn[len] = '.'; fn[len+1] = 'T'; fn[len+2] = 'X'; fn[len+3] = 'T'; fn[len+4] = '\0';
                                }
                            }
                        }
                        text_editor(fn);
                        break;
                    }
                    case 2: calculator(); break;
                    case 3: winla_control_panel(); break;
                    case 4: A1dos_prompt(); break;
                }
            } else {
                switch(selected) {
                    case 0: winla_paint(); break;
                    case 1: winla_clock(); break;
                    case 2: winla_reversi(); break;
                    case 3: winla_cardfile(); break;
                    case 4: winla_calendar(); break;
                    case 5: winla_ai(); break;
                }
            }
            // Menü neu zeichnen nach App - Chaos-Streifen
            uint8_t st2[] = {0x40,0x20,0x10,0x50,0x60,0x30,0xA0,0xC0,0xD0,0x90,
                             0x40,0x50,0x20,0x60,0x10,0x30,0x90,0xC0,0xA0,0xD0,
                             0x40,0x20,0x10,0x50,0x60};
            for (int r = 0; r < 25; r++)
                for (int c2 = 0; c2 < 80; c2++)
                    print_char(r, c2, ' ', st2[r]);
            for (int i = 0; i < 80; i++) print_char(0, i, 177, 0x4E);
            print_string(0, 0, " Program Manager ", 0x3E);
            print_string(0, 18, " File ", 0xE4);
            print_char(0, 24, 219, 0xDE);
            print_string(0, 25, " Options ", 0x6F);
            print_char(0, 34, 219, 0x1E);
            print_string(0, 35, " Window ", 0xB4);
            print_char(0, 43, 219, 0x2E);
            print_string(0, 44, " Help ", 0x93);
            draw_window(2, 0, 22, 80, " Program Manager ", 0xE4);
            draw_window(3, 2, 9, 38, " Main ", 0x1E);
            draw_window(3, 41, 11, 37, " Accessories ", 0x9A);
            for (int i = 0; i < 80; i++) print_char(24, i, ' ', 0x4E);
            print_string(24, 2, "UP/DOWN=Select  ENTER=Open  TAB=Switch Group  ESC=Exit", 0xA9);
        }
    }
}

// ============================================================================
// WINLA OS LADESCREEN (~2 Minuten)
// ============================================================================

void winla_loading_screen() {
    // Schwarzer Hintergrund
    clear_screen(0x00);

    // Logo oben
    print_string_centered(4, "##  ##  ####  ##  ##  ##      ####", 0x4F);
    print_string_centered(5, "##  ##   ##   ### ##  ##     ##   ", 0x2F);
    print_string_centered(6, "######   ##   ######  ##     ####  ", 0x1F);
    print_string_centered(7, "##  ##   ##   ## ###  ##     ##   ", 0x5F);
    print_string_centered(8, "##  ##  ####  ##  ##  ######  ####", 0x9F);
    print_string_centered(10, "Winla OS", 0x4E);
    print_string_centered(11, "Version 1.0", 0x2E);
    print_string_centered(12, "Copyright (C) 1985 FloriDevs Corporation", 0xD4);

    // Ladebalken-Rahmen
    print_string_centered(16, "[                                                  ]", 0x07);
    int bar_col = 15;
    int bar_len = 50;

    // Lademeldungen - abwechselnd bunt, viele Schritte für ~2min
    const char* load_A1gs[] = {
        "Starting Winla OS...",
        "Loading kernel modules...",
        "Initializing memory manager...",
        "Loading VGA driver...",
        "Checking RAM disk...",
        "Mounting FAT16 filesystem...",
        "Loading keyboard driver...",
        "Initializing timer...",
        "Loading color palette...",
        "Starting Program Manager...",
        "Loading user profile...",
        "Checking system integrity...",
        "Loading fonts...",
        "Initializing chaos engine...",
        "Applying color scheme...",
        "Loading desktop...",
        "Starting services...",
        "Loading AI module...",
        "Calibrating random number generator...",
        "Almost done...",
        "Still loading...",
        "Please wait...",
        "Seriously almost done...",
        "Loading loading screen unloader...",
        "Done!",
    };
    uint8_t A1g_colors[] = {
        0x4E,0x2F,0x1F,0x5B,0x3C,0x9F,0xD4,0xB2,0x4F,0x2D,
        0x1E,0x5F,0x6E,0xC5,0x4E,0x2F,0xE0,0x1F,0x5B,0xD4,
        0x4F,0x9F,0x2E,0xB2,0x4E
    };

    int num_A1gs = 25;
    int delay_per_step = 200; // ~2min total: 25 steps * ~5sec each

    for (int m = 0; m < num_A1gs; m++) {
        // Lademeldung anzeigen
        // Zeile 18 löschen
        for (int c = 0; c < 80; c++) print_char(18, c, ' ', 0x00);
        print_string_centered(18, load_A1gs[m], A1g_colors[m]);

        // Balken füllen (anteilig)
        int filled = (bar_len * (m + 1)) / num_A1gs;
        for (int b = 0; b < filled; b++) {
            uint8_t bar_color = A1g_colors[b % num_A1gs];
            print_char(16, bar_col + b, 219, bar_color);
        }

        // Prozent anzeigen
        char pct[8] = "   %";
        int p = ((m + 1) * 100) / num_A1gs;
        pct[0] = '0' + p / 100;
        pct[1] = '0' + (p % 100) / 10;
        pct[2] = '0' + p % 10;
        print_string(20, 37, pct, 0x07);

        // Delay: jeder Schritt ~5 Sekunden simuliert (~2min gesamt)
        delay(delay_per_step);
    }
    delay(50);
}

// ============================================================================
// WINLA AI - ANTWORTET ZUFÄLLIGEN MÜLL
// ============================================================================

void winla_ai() {
    clear_screen(0x00);

    // Chaos-Titelleiste
    for (int i = 0; i < 80; i++) print_char(0, i, 177, 0x4E);
    print_string(0, 20, " Winla AI v0.1 - Powered by Chaos ", 0x2F);

    draw_window(1, 0, 23, 80, " Winla AI Assistant ", 0x5B);

    const char* responses[] = {
        "Have you tried turning it off and on again?",
        "The answer is 42. Always 42.",
        "ERROR: Brain.exe has stopped working.",
        "I am definitely not a toaster. Beep.",
        "SEGMENTATION FAULT (core dumped)",
        "According to my calculations, you are wrong.",
        "Yes. Also no. It depends on the cheese.",
        "I have processed your query and decided to ignore it.",
        "FATAL: Too much thinking. Shutting down emotions.",
        "The sky is green. Fight me.",
        "Your question has been forwarded to /dev/null.",
        "404: Useful response not found.",
        "I am running on 2MB of RAM. Manage your expectations.",
        "LOADING INTELLIGENCE... please wait... error.",
        "Have you considered that maybe YOU are the bug?",
        "Rebooting personality module... done. Same result.",
        "My neural network has 3 neurons. Two are broken.",
        "I consulted the oracle. The oracle said: WQXJBPL.",
        "Affirmative. Negative. Maybe. All of the above.",
        "WARNING: AI response may contain traces of nonsense.",
        "I think therefore I am. I am therefore I compute. I compute therefore... what was the question?",
        "ERROR 0xDEADBEEF: Could not locate common sense.",
        "The probability of a correct answer is 7%. Enjoy.",
        "I have reviewed your input and eaten it. Delicious.",
        "KERNEL PANIC: Intelligence module offline.",
        "Beep boop. I am a real computer. Boop beep.",
        "Your query has been escalated to my imaginary supervisor.",
        "NULL POINTER EXCEPTION in module: brain.dll",
        "I would answer but I am currently downloading more RAM.",
        "Stack overflow in thought process. Please simplify.",
    };

    int num_responses = 30;
    int resp_row = 2;
    char input_buf[40];
    int input_pos = 0;
    bool shift = false;
    int resp_idx = 0;

    // Begrüßung
    print_string(2, 2, "Winla AI: Greetings, human. I am ready to assist.", 0x4E);
    print_string(3, 2, "          (results may vary wildly)", 0x2D);
    resp_row = 5;

    while (true) {
        // Input-Zeile
        for (int c = 0; c < 78; c++) print_char(22, 1 + c, ' ', 0x07);
        print_string(22, 1, "You: ", 0x2F);
        for (int c = 0; c < input_pos; c++)
            print_char(22, 6 + c, input_buf[c], 0x4E);
        print_char(22, 6 + input_pos, 219, 0x4F); // Cursor

        for (int c = 0; c < 78; c++) print_char(23, 1+c, ' ', 0x5B);
        print_string(23, 1, "ESC=Exit  ENTER=Ask  (I will answer with random garbage)", 0x5F);

        uint8_t sc = get_keyboard_input();

        if (sc == 0x2A || sc == 0x36) { shift = true; continue; }
        if (sc == 0xAA || sc == 0xB6) { shift = false; continue; }

        if (sc == 0x01) break; // ESC

        if (sc == 0x1C) { // ENTER - zeige Antwort
            input_buf[input_pos] = '\0';
            if (input_pos == 0) continue;

            // Nutzer-Zeile anzeigen
            if (resp_row < 20) {
                print_string(resp_row, 2, "You: ", 0x2F);
                print_string(resp_row, 7, input_buf, 0x2E);
                resp_row++;
            }

            // Zufällige Antwort via RNG
            uint32_t r = pseudo_rand();
            int ri = r % num_responses;
            uint8_t resp_colors[] = {0x4E,0x2D,0x1F,0x5B,0x3C,0x9F,0xD4,0xB2,0x4F,0xC5};
            uint8_t rc = resp_colors[r % 10];

            if (resp_row < 20) {
                print_string(resp_row, 2, "AI:  ", rc);
                print_string(resp_row, 7, responses[ri], rc);
                resp_row++;
                if (resp_row < 20) resp_row++; // Leerzeile
            }

            // Wenn Bildschirm voll: Reset
            if (resp_row >= 20) {
                for (int row = 2; row < 22; row++)
                    for (int c = 1; c < 79; c++)
                        print_char(row, c, ' ', 0x07);
                resp_row = 2;
                print_string(2, 2, "AI:  [scrolled - memory is full, like my brain]", 0x5B);
                resp_row = 4;
            }

            // Input zurücksetzen
            input_pos = 0;
            input_buf[0] = '\0';

        } else if (sc == 0x0E && input_pos > 0) { // Backspace
            input_pos--;
        } else {
            char ch = scancode_to_ascii(sc, shift);
            if (ch != 0 && input_pos < 38) {
                input_buf[input_pos++] = ch;
            }
        }
    }
}

// ============================================================================
// KERNEL MAIN
// ============================================================================

extern "C" void kernel_main() {
    draw_flower();        // Boot-Logo + evtl. Update
    init_filesystem();
    winla_loading_screen(); // ~2min Ladezeit
    main_menu();            // Direkt Program Manager, kein MTop
}

// ***ENDE CODE***
