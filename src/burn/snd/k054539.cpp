// copyright-holders:Aaron Giles
/*********************************************************

    Konami 054539 (TOP) PCM Sound Chip

    A lot of information comes from Amuse.
    Big thanks to them.

*********************************************************/

// IRQ handling is disabled for now...

#include "burnint.h"
#include "math.h"
#include "k054539.h"

static UINT32 nUpdateStep;
static INT32 nNumChips = 0;

typedef struct _k054539_interface k054539_interface;
struct _k054539_interface
{
	const char *rgnoverride;
	void (*apan)(double, double);
//	void (*irq)(running_machine *);
};

struct k054539_channel {
	UINT32 pos;
	UINT32 pfrac;
	INT32 val;
	INT32 pval;
};

struct k054539_info {
	k054539_interface intf;
	double voltab[256];
	double pantab[0xf];

	double k054539_gain[8];
	UINT8 k054539_posreg_latch[8][3];
	INT32 k054539_flags;

	UINT8 regs[0x230];
	UINT8 *ram;
	INT32 reverb_pos;

	INT32 cur_ptr;
	INT32 cur_limit;
	UINT8 *cur_zone;
	UINT8 *rom;
	UINT32 rom_size;
	UINT32 rom_mask;

	INT32 clock;
	
	double volume[2];
	INT32 output_dir[2];

	k054539_channel channels[8];
};

static k054539_info Chips[2];
static k054539_info *info;

static INT32 *soundbuf[2] = { NULL, NULL };
static INT16 *mixerbuf = NULL; // for native -> nBurnSoundRate conversion

void K054539_init_flags(INT32 chip, INT32 flags)
{
#if defined FBA_DEBUG
	if (!DebugSnd_K054539Initted) bprintf(PRINT_ERROR, _T("K054539_init_flags called without init\n"));
	if (chip > nNumChips) bprintf(PRINT_ERROR, _T("K054539_init_flags called with invalid chip %x\n"), chip);
#endif

	info = &Chips[chip];
	info->k054539_flags = flags;
}

void K054539_set_gain(INT32 chip, INT32 channel, double gain)
{
#if defined FBA_DEBUG
	if (!DebugSnd_K054539Initted) bprintf(PRINT_ERROR, _T("K054539_set_gain called without init\n"));
	if (chip > nNumChips) bprintf(PRINT_ERROR, _T("K054539_set_gain called with invalid chip %x\n"), chip);
#endif

	info = &Chips[chip];
	if (gain >= 0) info->k054539_gain[channel] = gain;
}

static INT32 k054539_regupdate()
{
#if defined FBA_DEBUG
	if (!DebugSnd_K054539Initted) bprintf(PRINT_ERROR, _T("K054539_regupdate called without init\n"));
#endif

	return !(info->regs[0x22f] & 0x80);
}

static void k054539_keyon(INT32 channel)
{
#if defined FBA_DEBUG
	if (!DebugSnd_K054539Initted) bprintf(PRINT_ERROR, _T("K054539_keyon called without init\n"));
#endif

	if(k054539_regupdate())
		info->regs[0x22c] |= 1 << channel;
}

static void k054539_keyoff(INT32 channel)
{
#if defined FBA_DEBUG
	if (!DebugSnd_K054539Initted) bprintf(PRINT_ERROR, _T("K054539_keyoff called without init\n"));
#endif

	if(k054539_regupdate())
		info->regs[0x22c] &= ~(1 << channel);
}

void K054539Write(INT32 chip, INT32 offset, UINT8 data)
{
#if defined FBA_DEBUG
	if (!DebugSnd_K054539Initted) bprintf(PRINT_ERROR, _T("K054539Write called without init\n"));
	if (chip > nNumChips) bprintf(PRINT_ERROR, _T("K054539Write called with invalid chip %x\n"), chip);
#endif

	info = &Chips[chip];

	INT32 latch, offs, ch, pan;
	UINT8 *regbase, *regptr, *posptr;

	regbase = info->regs;
	latch = (info->k054539_flags & K054539_UPDATE_AT_KEYON) && (regbase[0x22f] & 1);

	if (latch && offset < 0x100)
	{
		offs = (offset & 0x1f) - 0xc;
		ch = offset >> 5;

		if (offs >= 0 && offs <= 2)
		{
			// latch writes to the position index registers
			info->k054539_posreg_latch[ch][offs] = data;
			return;
		}
	}

	else switch(offset)
	{
		case 0x13f:
			pan = data >= 0x11 && data <= 0x1f ? data - 0x11 : 0x18 - 0x11;
			if(info->intf.apan)
				info->intf.apan(info->pantab[pan], info->pantab[0xe - pan]);
		break;

		case 0x214:
			if (latch)
			{
				for(ch=0; ch<8; ch++)
				{
					if(data & (1<<ch))
					{
						posptr = &info->k054539_posreg_latch[ch][0];
						regptr = regbase + (ch<<5) + 0xc;

						// update the chip at key-on
						regptr[0] = posptr[0];
						regptr[1] = posptr[1];
						regptr[2] = posptr[2];

						k054539_keyon(ch);
					}
				}
			}
			else
			{
				for(ch=0; ch<8; ch++)
					if(data & (1<<ch))
						k054539_keyon(ch);
			}
		break;

		case 0x215:
			for(ch=0; ch<8; ch++)
				if(data & (1<<ch))
					k054539_keyoff(ch);
		break;

		case 0x22d:
			if(regbase[0x22e] == 0x80)
				info->cur_zone[info->cur_ptr] = data;
			info->cur_ptr++;
			if(info->cur_ptr == info->cur_limit)
				info->cur_ptr = 0;
		break;

		case 0x22e:
			info->cur_zone =
				data == 0x80 ? info->ram :
				info->rom + 0x20000*data;
			info->cur_limit = data == 0x80 ? 0x4000 : 0x20000;
			info->cur_ptr = 0;
		break;

		default:
		break;
	}

	regbase[offset] = data;
}

UINT8 K054539Read(INT32 chip, INT32 offset)
{
#if defined FBA_DEBUG
	if (!DebugSnd_K054539Initted) bprintf(PRINT_ERROR, _T("K054539Read called without init\n"));
	if (chip > nNumChips) bprintf(PRINT_ERROR, _T("K054539Read called with invalid chip %x\n"), chip);
#endif

	info = &Chips[chip];

	switch(offset) {
	case 0x22d:
		if(info->regs[0x22f] & 0x10) {
			UINT8 res = info->cur_zone[info->cur_ptr];
			info->cur_ptr++;
			if(info->cur_ptr == info->cur_limit)
				info->cur_ptr = 0;
			return res;
		} else
			return 0;

	case 0x22c:
		break;

	default:
		break;
	}

	return info->regs[offset];
}

void K054539Reset(INT32 chip)
{
#if defined FBA_DEBUG
	if (!DebugSnd_K054539Initted) bprintf(PRINT_ERROR, _T("K054539Reset called without init\n"));
	if (chip > nNumChips) bprintf(PRINT_ERROR, _T("K054539Reset called with invalid chip %x\n"), chip);
#endif

	info = &Chips[chip];

	info->reverb_pos = 0;
	info->cur_ptr = 0;
	info->cur_zone = info->rom;
	memset(info->ram, 0, 0x4000*2+info->clock/50*2);
	memset(info->regs, 0, sizeof(info->regs));
	memset(info->k054539_posreg_latch, 0, sizeof(info->k054539_posreg_latch));
	memset(info->channels, 0, sizeof(info->channels));
}

static void k054539_init_chip(INT32 clock, UINT8 *rom, INT32 nLen)
{
	memset(info->regs, 0, sizeof(info->regs));
	memset(info->k054539_posreg_latch, 0, sizeof(info->k054539_posreg_latch));
	info->k054539_flags |= K054539_UPDATE_AT_KEYON; // make it default until proven otherwise

	// Real size of 0x4000, the addon is to simplify the reverb buffer computations
	info->ram = (UINT8*)BurnMalloc(0x4000*2+clock/50*2);
	info->reverb_pos = 0;
	info->cur_ptr = 0;
	memset(info->ram, 0, 0x4000*2+clock/50*2);

	info->rom = rom;
	info->rom_size = nLen;
	info->rom_mask = 0xffffffffU;
	for (INT32 i = 0; i < 32; i++) {
		if((1U<<i) >= info->rom_size) {
			info->rom_mask = (1U<<i) - 1;
			break;
		}
	}

	info->volume[BURN_SND_K054539_ROUTE_1] = 1.00;
	info->volume[BURN_SND_K054539_ROUTE_2] = 1.00;
	info->output_dir[BURN_SND_K054539_ROUTE_1] = BURN_SND_ROUTE_BOTH;
	info->output_dir[BURN_SND_K054539_ROUTE_2] = BURN_SND_ROUTE_BOTH;

	//	if(info->intf->irq)
//		timer_pulse(ATTOTIME_IN_HZ(480), info, 0, k054539_irq); // 10% of usual clock...
}

void K054539SetApanCallback(INT32 chip, void (*ApanCB)(double, double))
{
	info = &Chips[chip];
	info->intf.apan = ApanCB;
}

void K054539Init(INT32 chip, INT32 clock, UINT8 *rom, INT32 nLen)
{
	DebugSnd_K054539Initted = 1;
	
	INT32 i;

	memset(&Chips[chip], 0, sizeof(k054539_info));

	info = &Chips[chip];

	info->clock = clock;

	nUpdateStep = (INT32)(((float)clock / nBurnSoundRate) * 32768);

	for (i = 0; i < 8; i++)
		info->k054539_gain[i] = 1.0;

	info->k054539_flags = K054539_RESET_FLAGS;

	for(i=0; i<256; i++)
		info->voltab[i] = pow(10.0, (-36.0 * (double)i / (double)0x40) / 20.0) / 4.0;

	for(i=0; i<0xf; i++)
		info->pantab[i] = sqrt((double)i)  / sqrt((double)0xe);

	k054539_init_chip(clock, rom, nLen);

	if (soundbuf[0] == NULL) soundbuf[0] = (INT32*)BurnMalloc(nBurnSoundLen * sizeof(INT32));
	if (soundbuf[1] == NULL) soundbuf[1] = (INT32*)BurnMalloc(nBurnSoundLen * sizeof(INT32));

	if (mixerbuf == NULL) mixerbuf = (INT16 *)BurnMalloc(clock * 2 * sizeof(INT16));

	nNumChips = chip;
}

void K054539SetRoute(INT32 chip, INT32 nIndex, double nVolume, INT32 nRouteDir)
{
#if defined FBA_DEBUG
	if (!DebugSnd_K054539Initted) bprintf(PRINT_ERROR, _T("K054539SetRoute called without init\n"));
	if (chip >nNumChips) bprintf(PRINT_ERROR, _T("K054539SetRoute called with invalid chip %x\n"), chip);
	if (nIndex < 0 || nIndex > 1) bprintf(PRINT_ERROR, _T("K054539SetRoute called with invalid index %i\n"), nIndex);
#endif

	info = &Chips[chip];
	
	info->volume[nIndex] = nVolume;
	info->output_dir[nIndex] = nRouteDir;
}

void K054539Exit()
{
#if defined FBA_DEBUG
	if (!DebugSnd_K054539Initted) bprintf(PRINT_ERROR, _T("K054539Exit called without init\n"));
#endif

	if (!DebugSnd_K054539Initted) return;

	BurnFree (soundbuf[0]);
	BurnFree (soundbuf[1]);
	soundbuf[0] = NULL;
	soundbuf[1] = NULL;

	BurnFree (mixerbuf);
	mixerbuf = NULL;

	for (INT32 i = 0; i < 2; i++) {
		info = &Chips[i];
		BurnFree (info->ram);
	}
	
	DebugSnd_K054539Initted = 0;
	nNumChips = 0;
}

void K054539Update(INT32 chip, INT16 *outputs, INT32 samples_len) //INT16 *pBuf, INT32 length)
{
#if defined FBA_DEBUG
	if (!DebugSnd_K054539Initted) bprintf(PRINT_ERROR, _T("K054539Update called without init\n"));
	if (chip > nNumChips) bprintf(PRINT_ERROR, _T("K054539Update called with invalid chip %x\n"), chip);
#endif

	info = &Chips[chip];
#define VOL_CAP 1.80

	static const INT16 dpcm[16] = {
		0<<8, 1<<8, 4<<8, 9<<8, 16<<8, 25<<8, 36<<8, 49<<8,
		-64<<8, -49<<8, -36<<8, -25<<8, -16<<8, -9<<8, -4<<8, -1<<8
	};

	INT16 *rbase = (INT16 *)(info->ram);
	UINT8 *rom = info->rom;
	UINT32 rom_mask = info->rom_mask;

	if(!(info->regs[0x22f] & 1))
		return;

	// re-sampleizer pt.1
	INT32 length = (((((48000 * 1000) / nBurnFPS) * samples_len) / nBurnSoundLen)) / 10;
	INT16 *pBuf = mixerbuf;

	memset(mixerbuf, 0, nBurnSoundLen * 2 * sizeof(INT16));
	if (length > (info->clock * 100) / nBurnFPS) length = (info->clock * 100) / nBurnFPS;

	for(int sample = 0; sample < length; sample++) {
		double lval, rval;

		if(!(info->k054539_flags & K054539_DISABLE_REVERB))
			lval = rval = rbase[info->reverb_pos];
		else
			lval = rval = 0;
		rbase[info->reverb_pos] = 0;

		for(int ch=0; ch<8; ch++)
			if(info->regs[0x22c] & (1<<ch)) {
				unsigned char *base1 = info->regs + 0x20*ch;
				unsigned char *base2 = info->regs + 0x200 + 0x2*ch;
				struct k054539_channel *chan = info->channels + ch;

				int delta = base1[0x00] | (base1[0x01] << 8) | (base1[0x02] << 16);

				int vol = base1[0x03];

				int bval = vol + base1[0x04];
				if (bval > 255)
					bval = 255;

				int pan = base1[0x05];
				// DJ Main: 81-87 right, 88 middle, 89-8f left
				if (pan >= 0x81 && pan <= 0x8f)
					pan -= 0x81;
				else if (pan >= 0x11 && pan <= 0x1f)
					pan -= 0x11;
				else
					pan = 0x18 - 0x11;

				double cur_gain = info->k054539_gain[ch];

				double lvol = info->voltab[vol] * info->pantab[pan] * cur_gain;
				if (lvol > VOL_CAP)
					lvol = VOL_CAP;

				double rvol = info->voltab[vol] * info->pantab[0xe - pan] * cur_gain;
				if (rvol > VOL_CAP)
					rvol = VOL_CAP;

				double rbvol= info->voltab[bval] * cur_gain / 2;
				if (rbvol > VOL_CAP)
					rbvol = VOL_CAP;

				int rdelta = (base1[6] | (base1[7] << 8)) >> 3;
				rdelta = (rdelta + info->reverb_pos) & 0x3fff;

				int cur_pos = (base1[0x0c] | (base1[0x0d] << 8) | (base1[0x0e] << 16)) & rom_mask;

				int fdelta, pdelta;
				if(base2[0] & 0x20) {
					delta = -delta;
					fdelta = +0x10000;
					pdelta = -1;
				} else {
					fdelta = -0x10000;
					pdelta = +1;
				}

				int cur_pfrac, cur_val, cur_pval;
				if(cur_pos != (INT32)chan->pos) {
					chan->pos = cur_pos;
					cur_pfrac = 0;
					cur_val = 0;
					cur_pval = 0;
				} else {
					cur_pfrac = chan->pfrac;
					cur_val = chan->val;
					cur_pval = chan->pval;
				}

				switch(base2[0] & 0xc) {
				case 0x0: { // 8bit pcm
					cur_pfrac += delta;
					while(cur_pfrac & ~0xffff) {
						cur_pfrac += fdelta;
						cur_pos += pdelta;

						cur_pval = cur_val;
						cur_val = (INT16)(rom[cur_pos] << 8);
						if(cur_val == (INT16)0x8000 && (base2[1] & 1)) {
							cur_pos = (base1[0x08] | (base1[0x09] << 8) | (base1[0x0a] << 16)) & rom_mask;
							cur_val = (INT16)(rom[cur_pos] << 8);
						}
						if(cur_val == (INT16)0x8000) {
							k054539_keyoff(ch);
							cur_val = 0;
							break;
						}
					}
					break;
				}

				case 0x4: { // 16bit pcm lsb first
					pdelta <<= 1;

					cur_pfrac += delta;
					while(cur_pfrac & ~0xffff) {
						cur_pfrac += fdelta;
						cur_pos += pdelta;

						cur_pval = cur_val;
						cur_val = (INT16)(rom[cur_pos] | rom[cur_pos+1]<<8);
						if(cur_val == (INT16)0x8000 && (base2[1] & 1)) {
							cur_pos = (base1[0x08] | (base1[0x09] << 8) | (base1[0x0a] << 16)) & rom_mask;
							cur_val = (INT16)(rom[cur_pos] | rom[cur_pos+1]<<8);
						}
						if(cur_val == (INT16)0x8000) {
							k054539_keyoff(ch);
							cur_val = 0;
							break;
						}
					}
					break;
				}

				case 0x8: { // 4bit dpcm
					cur_pos <<= 1;
					cur_pfrac <<= 1;
					if(cur_pfrac & 0x10000) {
						cur_pfrac &= 0xffff;
						cur_pos |= 1;
					}

					cur_pfrac += delta;
					while(cur_pfrac & ~0xffff) {
						cur_pfrac += fdelta;
						cur_pos += pdelta;

						cur_pval = cur_val;
						cur_val = rom[cur_pos>>1];
						if(cur_val == 0x88 && (base2[1] & 1)) {
							cur_pos = ((base1[0x08] | (base1[0x09] << 8) | (base1[0x0a] << 16)) & rom_mask) << 1;
							cur_val = rom[cur_pos>>1];
						}
						if(cur_val == 0x88) {
							k054539_keyoff(ch);
							cur_val = 0;
							break;
						}
						if(cur_pos & 1)
							cur_val >>= 4;
						else
							cur_val &= 15;
						cur_val = cur_pval + dpcm[cur_val];
						if(cur_val < -32768)
							cur_val = -32768;
						else if(cur_val > 32767)
							cur_val = 32767;
					}

					cur_pfrac >>= 1;
					if(cur_pos & 1)
						cur_pfrac |= 0x8000;
					cur_pos >>= 1;
					break;
				}
				default:
					//LOG(("Unknown sample type %x for channel %d\n", base2[0] & 0xc, ch));
					break;
				}
				lval += cur_val * lvol;
				rval += cur_val * rvol;
				rbase[(rdelta + info->reverb_pos) & 0x1fff] += INT16(cur_val*rbvol);

				chan->pos = cur_pos;
				chan->pfrac = cur_pfrac;
				chan->pval = cur_pval;
				chan->val = cur_val;

				if(k054539_regupdate()) {
					base1[0x0c] = cur_pos     & 0xff;
					base1[0x0d] = cur_pos>> 8 & 0xff;
					base1[0x0e] = cur_pos>>16 & 0xff;
				}
			}
		info->reverb_pos = (info->reverb_pos + 1) & 0x1fff;

		INT32 nLeftSample = 0, nRightSample = 0;
		
		if ((info->output_dir[BURN_SND_K054539_ROUTE_1] & BURN_SND_ROUTE_LEFT) == BURN_SND_ROUTE_LEFT) {
			nLeftSample += (INT32)(lval * info->volume[BURN_SND_K054539_ROUTE_1]);
		}
		if ((info->output_dir[BURN_SND_K054539_ROUTE_1] & BURN_SND_ROUTE_RIGHT) == BURN_SND_ROUTE_RIGHT) {
			nRightSample += (INT32)(rval * info->volume[BURN_SND_K054539_ROUTE_1]);
		}
		
		if ((info->output_dir[BURN_SND_K054539_ROUTE_2] & BURN_SND_ROUTE_LEFT) == BURN_SND_ROUTE_LEFT) {
			nLeftSample += (INT32)(lval * info->volume[BURN_SND_K054539_ROUTE_2]);
		}
		if ((info->output_dir[BURN_SND_K054539_ROUTE_2] & BURN_SND_ROUTE_RIGHT) == BURN_SND_ROUTE_RIGHT) {
			nRightSample += (INT32)(rval * info->volume[BURN_SND_K054539_ROUTE_2]);
		}
		
		pBuf[0] = BURN_SND_CLIP(nLeftSample);
		pBuf[1] = BURN_SND_CLIP(nRightSample);
		pBuf += 2;
	}

	// re-sampleizer pt.2
	for (INT32 j = 0; j < samples_len; j++)
	{
		INT32 k = ((((48000000 / nBurnFPS) * j) / nBurnSoundLen)) / 10;

		outputs[0] = BURN_SND_CLIP(mixerbuf[k*2+0] + outputs[0]);
		outputs[1] = BURN_SND_CLIP(mixerbuf[k*2+1] + outputs[1]);
		outputs += 2;
	}
}

INT32 K054539Scan(INT32 nAction)
{
#if defined FBA_DEBUG
	if (!DebugSnd_K054539Initted) bprintf(PRINT_ERROR, _T("K054539Scan called without init\n"));
#endif

	struct BurnArea ba;
	char szName[32];

	if ((nAction & ACB_DRIVER_DATA) == 0) {
		return 1;
	}

	for (INT32 i = 0; i < nNumChips+1; i++) {
		info = &Chips[i];

		sprintf(szName, "K054539 Latch %d", i);
		ba.Data		= info->k054539_posreg_latch;
		ba.nLen		= 8*3;
		ba.nAddress = 0;
		ba.szName	= szName;
		BurnAcb(&ba);

		sprintf(szName, "K054539 Regs # %d", i);
		ba.Data		= info->regs;
		ba.nLen		= 0x230;
		ba.nAddress = 0;
		ba.szName	= szName;
		BurnAcb(&ba);

		sprintf(szName, "K054539 Ram # %d", i);
		ba.Data		= info->ram;
		ba.nLen		= 0x4000*2+info->clock/50*2;
		ba.nAddress = 0;
		ba.szName	= szName;
		BurnAcb(&ba);

		sprintf(szName, "K054539 Channels # %d", i);
		ba.Data		= &info->channels;
		ba.nLen		= sizeof(k054539_channel) * 8;
		ba.nAddress = 0;
		ba.szName	= szName;
		BurnAcb(&ba);

		SCAN_VAR(info->k054539_flags);
		SCAN_VAR(info->reverb_pos);
		SCAN_VAR(info->cur_ptr);
		SCAN_VAR(info->cur_limit);

		if (nAction & ACB_WRITE) {
			INT32 data = info->regs[0x22e];
			info->cur_zone =
				data == 0x80 ? info->ram :
				info->rom + 0x20000*data;
			info->cur_limit = data == 0x80 ? 0x4000 : 0x20000;
		}
	}

	return 0;
}
