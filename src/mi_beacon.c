/*
 * mi_beacon.c
 *
 *  Created on: 16.02.2021
 *      Author: pvvx
 */
#include "tl_common.h"
#if USE_MIHOME_BEACON
#include "app_config.h"
#include "ble.h"
#include "app.h"
#include "battery.h"
#include "trigger.h"
#if (DEV_SERVICES & SERVICE_RDS)
#include "rds_count.h"
#endif
#include "mi_beacon.h"
#include "ccm.h"

#if (DEV_SERVICES & SERVICE_BINDKEY)

/* Encrypted mi nonce */
typedef struct __attribute__((packed)) _mi_beacon_nonce_t{
    u8  mac[6];
	u16 pid;
	union {
		struct {
			u8  cnt;
			u8  ext_cnt[3];
		};
		u32 cnt32;
    };
} mi_beacon_nonce_t, * pmi_beacon_nonce_t;

//// Init data
// RAM u8 bindkey[16];
RAM mi_beacon_nonce_t beacon_nonce;
/// Vars
typedef struct _mi_beacon_data_t { // out data
	s16 temp;	// x0.1 C
	u16 humi;	// x0.1 %
	u8 batt;	// 0..100 %
	u8 stage;
} mi_beacon_data_t;
RAM mi_beacon_data_t mi_beacon_data;

typedef struct _summ_data_t { // calk summ data
	u32	batt; // mv
	s32	temp; // x 0.01 C
	u32	humi; // x 0.01 %
	u32 count;
} mib_summ_data_t;
RAM mib_summ_data_t mib_summ_data;

/* Initializing data for mi beacon */
void mi_beacon_init(void) {
	memcpy(beacon_nonce.mac, mac_public, 6);
	beacon_nonce.pid = XIAOMI_DID;
}

/* Averaging measurements */
_attribute_ram_code_
void mi_beacon_summ(void) {
	if(mib_summ_data.count > 0x7fff) {
		memset(&mib_summ_data, 0, sizeof(mib_summ_data));
	}
	mib_summ_data.temp += measured_data.temp;
	mib_summ_data.humi += measured_data.humi;
#if USE_AVERAGE_BATTERY
	mib_summ_data.batt += measured_data.average_battery_mv;
#else
	mib_summ_data.batt += measured_data.battery_mv;
#endif
	mib_summ_data.count++;
}

/* Create encrypted mi beacon packet */
__attribute__((optimize("-Os")))
void mi_encrypt_data_beacon(void) {
	beacon_nonce.cnt32 = adv_buf.send_count;
	adv_buf.update_count = -1; // next call only at next measurement
	if (++mi_beacon_data.stage > 2) {
		mi_beacon_data.stage = 0;
		if(mib_summ_data.count) {
			mi_beacon_data.temp = ((s16)(mib_summ_data.temp/(s32)mib_summ_data.count) + 5)/10;
			mi_beacon_data.humi = ((u16)(mib_summ_data.humi/mib_summ_data.count)  + 5)/10;
			mi_beacon_data.batt = get_battery_level((u16)(mib_summ_data.batt/mib_summ_data.count));
			memset(&mib_summ_data, 0, sizeof(mib_summ_data));
		} else {
			mi_beacon_data.temp = measured_data.temp_x01;
			mi_beacon_data.humi = measured_data.humi_x01;
			mi_beacon_data.batt = measured_data.battery_level;
		}
	}
	padv_mi_mac_beacon_t p = (padv_mi_mac_beacon_t)&adv_buf.data;
	p->head.uid = GAP_ADTYPE_SERVICE_DATA_UUID_16BIT; // 16-bit UUID
	p->head.UUID = ADV_XIAOMI_UUID16; // 16-bit UUID for Members 0xFE95 Xiaomi Inc.
	p->head.dev_id = beacon_nonce.pid;
	p->head.counter = beacon_nonce.cnt;
	adv_mi_data_t data;
	memcpy(p->MAC, mac_public, 6);
	switch (mi_beacon_data.stage) {
		case 0:
			data.id = XIAOMI_DATA_ID_Temperature; // XIAOMI_DATA_ID
			data.size = 2;
			data.data_i16 = mi_beacon_data.temp;	// Temperature, Range: -400..+1000 (x0.1 C)
			break;
		case 1:
			data.id = XIAOMI_DATA_ID_Humidity; // byte XIAOMI_DATA_ID
			data.size = 2;
			data.data_u16 = mi_beacon_data.humi; // Humidity percentage, Range: 0..1000 (x0.1 %)
			break;
		case 2:
			data.id = XIAOMI_DATA_ID_Power; // XIAOMI_DATA_ID
			data.size = 1;
			data.data_u8 = mi_beacon_data.batt; // Battery percentage, Range: 0..100 %
			break;
/*
		case 3:
#if 0
			p->head.fctrl.word = 0;
			p->head.fctrl.bit.MACInclude = 1;
			p->head.fctrl.bit.CapabilityInclude = 1;
			p->head.fctrl.bit.registered = 1;
			p->head.fctrl.bit.AuthMode = 2;
			p->head.fctrl.bit.version = 5; // XIAOMI_DEV_VERSION
#else
			p->head.fctrl.word = 0x5830; // 0x5830
#endif
			p->capability = 0x08; // capability
			p->head.size = sizeof(p->head) - sizeof(p->head.size) + sizeof(p->MAC) + sizeof(p->capability);
			return;
*/
	}
#if 0
	p->head.fctrl.word = 0;
	p->head.fctrl.bit.isEncrypted = 1;
	p->head.fctrl.bit.MACInclude = 1;
	p->head.fctrl.bit.ObjectInclude = 1;
	p->head.fctrl.bit.registered = 1;
	p->head.fctrl.bit.AuthMode = 2;
	p->head.fctrl.bit.version = 5;
#else
	p->head.fctrl.word = 0x5858; // 0x5858 version = 5, encrypted, MACInclude, ObjectInclude
#endif
	p->head.size = data.size + sizeof(p->head) - sizeof(p->head.size) + sizeof(p->MAC) + sizeof(p->data.id) + sizeof(p->data.size) + sizeof(beacon_nonce.ext_cnt) + 4; //size data + size head + size MAC + size data head + size counter bit8..31 bits + size mic 32 bits - 1
	u8 * pmic = (u8 *)p;
	pmic += data.size + sizeof(p->head) + sizeof(p->MAC) + sizeof(p->data.id) + sizeof(p->data.size);
	*pmic++ = beacon_nonce.ext_cnt[0];
	*pmic++ = beacon_nonce.ext_cnt[1];
	*pmic++ = beacon_nonce.ext_cnt[2];
    u8 aad = 0x11;
	aes_ccm_encrypt_and_tag((const unsigned char *)&bindkey, // указатель на ключ, аналог от MiHome
						   (u8*)&beacon_nonce, sizeof(beacon_nonce), // MAC и всякие ID устройства, 32-х битный счетчик
						   &aad, sizeof(aad),  // add = 0x11
						   (u8 *)&data, data.size + sizeof(p->data.id) + sizeof(p->data.size), // + size data head,  указатель на шифруемые данные
						   (u8 *)&p->data, // указатель куда писать результат
						   pmic, 4); // указатель куда писать типа подпись-"контрольную сумму" шифра
}
#if (DEV_SERVICES & SERVICE_RDS)
/* n - RDS_TYPES */
void mi_encrypt_event_beacon(u8 n) {
	padv_mi_cr_ev1_t p = (padv_mi_cr_ev1_t)&adv_buf.data;
	u8 * pmic;
	p->head.uid = GAP_ADTYPE_SERVICE_DATA_UUID_16BIT; // 16-bit UUID
	p->head.UUID = ADV_XIAOMI_UUID16; // 16-bit UUID for Members 0xFE95 Xiaomi Inc.
#if 0
	p->head.fctrl.word = 0;
	p->head.fctrl.bit.isEncrypted = 1;
	p->head.fctrl.bit.MACInclude = 0;
	p->head.fctrl.bit.ObjectInclude = 1;
	p->head.fctrl.bit.registered = 1;
	p->head.fctrl.bit.AuthMode = 2;
	p->head.fctrl.bit.version = 5;
#else
	p->head.fctrl.word = 0x5848; // 0x5848 version = 5, encrypted, no MACInclude, ObjectInclude, AuthMode 2
#endif
	p->head.dev_id = XIAOMI_DID;
	p->head.counter = (u8)adv_buf.send_count;
	if (n == RDS_SWITCH) {
		p->head.size = sizeof(adv_mi_cr_ev1_t) - sizeof(p->head.size);
		p->data.id = XIAOMI_DATA_ID_DoorSensor;
		p->data.size = sizeof(p->data.value);
		p->data.value = ! trg.flg.rds1_input;
		adv_buf.data_size = sizeof(adv_mi_cr_ev1_t);
		pmic = p->cnt;
	} else {
		padv_mi_cr_ev2_t p = (padv_mi_cr_ev2_t)&adv_buf.data;
		p->head.size = sizeof(adv_mi_cr_ev2_t) - sizeof(p->head.size);
		p->data.id = XIAOMI_VATR_ID_Count;
		p->data.size = sizeof(p->data.value);
		p->data.value = rds.count1;
		adv_buf.data_size = sizeof(adv_mi_cr_ev2_t);
		pmic = p->cnt;
	}
	beacon_nonce.cnt32 = adv_buf.send_count;
	*pmic++ = beacon_nonce.ext_cnt[0];
	*pmic++ = beacon_nonce.ext_cnt[1];
	*pmic++ = beacon_nonce.ext_cnt[2];
    u8 aad = 0x11;
	aes_ccm_encrypt_and_tag((const unsigned char *)&bindkey,
						   (u8*)&beacon_nonce, sizeof(beacon_nonce),
						   &aad, sizeof(aad),
						   (u8 *)&p->data, adv_buf.data_size - sizeof(p->head) - 7,
						   (u8 *)&p->data,
						   pmic, 4);
}
#endif // (DEV_SERVICES & SERVICE_RDS)
#endif // #if (DEV_SERVICES & SERVICE_BINDKEY)

/* Create mi beacon packet */
_attribute_ram_code_ __attribute__((optimize("-Os")))
void mi_data_beacon(void) {
	padv_mi_mac_beacon_t p = (padv_mi_mac_beacon_t)&adv_buf.data;
	memcpy(p->MAC, mac_public, 6);
	p->head.uid = GAP_ADTYPE_SERVICE_DATA_UUID_16BIT; // 16-bit UUID
	p->head.UUID = ADV_XIAOMI_UUID16; // 16-bit UUID for Members 0xFE95 Xiaomi Inc.
#if 0
	p->head.fctrl.word = 0;
	p->head.fctrl.bit.isEncrypted = 0;
	p->head.fctrl.bit.MACInclude = 1;
	p->head.fctrl.bit.ObjectInclude = 1;
	p->head.fctrl.bit.registered = 1;
	p->head.fctrl.bit.AuthMode = 2;
	p->head.fctrl.bit.version = 5;
#else
	p->head.fctrl.word = 0x5850; // 0x5850 version = 5, not encrypted, MACInclude, ObjectInclude, AuthMode 2
#endif
	p->head.dev_id = XIAOMI_DID;
	if (adv_buf.call_count < cfg.measure_interval) {
		p->data.id = XIAOMI_DATA_ID_TempAndHumidity;
		p->data.size = 4;
		p->data.t0d.temperature = measured_data.temp_x01; // x0.1 C
		p->data.t0d.humidity = measured_data.humi_x01; // x0.1 %
	} else {
		adv_buf.call_count = 1;
		adv_buf.send_count++;
		p->data.id = XIAOMI_DATA_ID_Power;
		p->data.size = 1;
		p->data.t0a.battery_level = measured_data.battery_level; // Battery percentage, Range: 0-100
	}
	p->head.counter = (u8)adv_buf.send_count;
	p->head.size = p->data.size + sizeof(p->head) - sizeof(p->head.size) + sizeof(p->MAC) + sizeof(p->data.id) + sizeof(p->data.size);
}

#if (DEV_SERVICES & SERVICE_RDS)
/* Create mi event beacon packet */
__attribute__((optimize("-Os")))
void mi_event_beacon(u8 n){
	padv_mi_ev1_t p = (padv_mi_ev1_t)&adv_buf.data;
	p->head.uid = GAP_ADTYPE_SERVICE_DATA_UUID_16BIT; // 16-bit UUID
	p->head.UUID = ADV_XIAOMI_UUID16; // 16-bit UUID for Members 0xFE95 Xiaomi Inc.
#if 0
	p->head.fctrl.word = 0;
	p->head.fctrl.bit.isEncrypted = 0;
	p->head.fctrl.bit.MACInclude = 0;
	p->head.fctrl.bit.ObjectInclude = 1;
	p->head.fctrl.bit.registered = 1;
	p->head.fctrl.bit.AuthMode = 2;
	p->head.fctrl.bit.version = 5;
#else
	p->head.fctrl.word = 0x5840; // 0x5840 version = 5, not encrypted, no MACInclude, ObjectInclude, AuthMode 2
#endif
	p->head.dev_id = XIAOMI_DID;
	p->head.counter = (u8)adv_buf.send_count;
	if (n == RDS_SWITCH) {
		p->head.size = sizeof(adv_mi_ev1_t) - sizeof(p->head.size);
		p->data.id = XIAOMI_DATA_ID_DoorSensor;
		p->data.size = sizeof(p->data.value);
		p->data.value = ! trg.flg.rds1_input;
		adv_buf.data_size = sizeof(adv_mi_ev1_t);
	} else {
		padv_mi_ev2_t p = (padv_mi_ev2_t)&adv_buf.data;
		p->head.size = sizeof(adv_mi_ev2_t) - sizeof(p->head.size);
		p->data.id = XIAOMI_VATR_ID_Count;
		p->data.size = sizeof(p->data.value);
		p->data.value = rds.count1;
		adv_buf.data_size = sizeof(adv_mi_ev2_t);
	}
}
#endif // #if (DEV_SERVICES & SERVICE_RDS)

#endif // USE_MIHOME_BEACON
