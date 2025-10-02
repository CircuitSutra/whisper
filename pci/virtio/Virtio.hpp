#pragma once

#include <iostream>
#include <linux/pci_regs.h>
#include <linux/virtio_pci.h>
#include <condition_variable>
#include <thread>
#include <span>
#include "../PciDev.hpp"

#define PCI_DEVICE_ID_VIRTIO_BASE		0x1040
#define PCI_SUBSYS_ID_VIRTIO_BASE		0x0040

#define PCI_VENDOR_ID_REDHAT_QUMRANET		0x1af4
#define PCI_SUBSYSTEM_VENDOR_ID_REDHAT_QUMRANET	0x1af4

#define VIRTQ_SIZE                              32
#define VIRTQ_USED_F_NO_NOTIFY                  1
#define VIRTQ_AVAIL_F_NO_INTERRUPT              1
/* This marks a buffer as continuing via the next field. */
#define VIRTQ_DESC_F_NEXT                       1
/* This marks a buffer as write-only (otherwise read-only). */
#define VIRTQ_DESC_F_WRITE                      2
/* This means the buffer contains a list of buffer descriptors. */
#define VIRTQ_DESC_F_INDIRECT                   4

namespace msix {
  struct cap;
  struct msix_table_entry;
  struct pba_table_entry;
}

class Virtio : public PciDev {

  public:

    // can probably just use included header, but let's not
    struct cap {
      uint8_t cap;
      uint8_t next;
      uint8_t len;
      uint8_t type;
      uint8_t bar;
      std::array<uint8_t, 3> padding;
      uint32_t cfg_offset;
      uint32_t cfg_length;
    } __attribute__ ((packed));


    struct common_cfg {
      uint32_t device_feature_select;
      uint32_t device_feature;
      uint32_t driver_feature_select;
      uint32_t driver_feature;
      uint16_t msix_config;
      uint16_t num_queues;
      uint8_t device_status;
      uint8_t config_generation;

      uint16_t queue_select;
      uint16_t queue_size;
      uint16_t queue_msix_vector;
      uint16_t queue_enable;
      uint16_t queue_notify_off;
      uint32_t queue_desc_lo;
      uint32_t queue_desc_hi;
      uint32_t queue_avail_lo;
      uint32_t queue_avail_hi;
      uint32_t queue_used_lo;
      uint32_t queue_used_hi;
    } __attribute__ ((packed));


    struct notify_cap {
      struct cap cap{};
      uint32_t notify_off_multiplier = 0;
    } __attribute__ ((packed));


    struct notify_cfg {
      uint32_t notify = 0;
    } __attribute__ ((packed));


    struct pci_cap {
      struct cap cap;
      std::array<uint8_t, 4> pci_cfg_data;
    } __attribute__ ((packed));

    struct virtqueue {
      // The virtring lives in host memory.
      struct descriptor {
        uint64_t address;
        uint32_t length;
        uint16_t flags;
        uint16_t next;
      } __attribute__((packed));

      struct avail_ring {
        uint16_t flags;
        uint16_t idx;
        std::array<uint16_t, VIRTQ_SIZE> ring;
      } __attribute__((packed));

      struct used_ring {
        uint16_t flags;
        uint16_t idx;
        struct elem {
          uint32_t idx;
          uint32_t len;
        };
        std::array<elem, VIRTQ_SIZE> ring;
      } __attribute__((packed));

      uint16_t size = VIRTQ_SIZE;
      uint16_t msix_vector = VIRTIO_MSI_NO_VECTOR;
      uint16_t enable = 0;

      uint64_t desc_addr = 0UL;
      uint64_t avail_addr = 0UL;
      uint64_t used_addr = 0UL;
      uint16_t last_avail_idx = 0;
    };

    Virtio(unsigned subsys_id, unsigned class_code, unsigned num_queues);

    ~Virtio() override = default;

    bool setup() override;

    virtual void operator()(unsigned vq) = 0;

    void reset();

    void interrupts();

  protected:

    // Don't need to write to notify because we're using MSI-X!
    void signal_used(unsigned num, const std::vector<virtqueue::used_ring::elem>& elems);

    void signal_config();

    // Returns true if successful.
    bool get_descriptors(unsigned num, std::vector<virtqueue::descriptor>& read, std::vector<virtqueue::descriptor>& write, unsigned& head, bool& finished);

    auto& get_vq(unsigned num)
    { return vqs_.at(num); }

    // Driver should use this to kick off an operation
    void notify(unsigned vq)
    { (*this)(vq); }

    uint64_t features_;
    uint8_t* device_cfg_ = nullptr;

  private:

    // Allocate one of each necessary cap for VIRTIO as well as corresponding datas.
    bool allocate_caps(uint32_t& common_cap_offset);

    void initialize_header();

    const unsigned subsys_id_;
    const unsigned class_code_;
    const unsigned num_queues_;

    cap* common_cap_ = nullptr;
    common_cfg* common_cfg_ = nullptr;
    notify_cap* notify_cap_ = nullptr;
    notify_cfg* notify_cfg_ = nullptr;
    // reading the ISR clears it, but we don't need to care if we use MSIs
    cap* isr_cap_ = nullptr;
    uint32_t* isr_cfg_ = nullptr;
    cap* device_cap_ = nullptr;

    msix::cap* msix_cap_ = nullptr;
    msix::msix_table_entry* msix_table_ = nullptr;
    msix::pba_table_entry* pba_table_ = nullptr;

    // For driver configuration
    uint32_t device_feature_selector_ = 0U;
    uint16_t config_msix_vector_ = 0U;
    uint16_t queue_selector_ = 0;
    std::vector<virtqueue> vqs_;
};
