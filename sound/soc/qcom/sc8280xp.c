// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2022, Linaro Limited

#include <dt-bindings/sound/qcom,q6afe.h>
#include <linux/clk.h>
#include <linux/cleanup.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <linux/soundwire/sdw.h>
#include <sound/jack.h>
#include <sound/control.h>
#include <sound/tlv.h>
#include <linux/input-event-codes.h>
#include "qdsp6/q6afe.h"
#include "qdsp6/q6apm-voice-backend.h"
#include "common.h"
#include "sdw.h"

#define SC8280XP_TDM_ACTIVE_SLOTS	2

struct sc8280xp_snd_data {
	bool stream_prepared[AFE_PORT_MAX];
	struct snd_soc_card *card;
	/* Optional, for boards whose TDM interface needs a HLOS clock vote */
	struct clk *tdm_vote;
	struct clk *tdm_bclk;
	/* TDM backend layout, from DT (defaults: 2 x 32 bit I2S at 48 kHz) */
	u32 tdm_slots;
	u32 tdm_slot_width;
	u32 tdm_rate;
	unsigned int tdm_fmt;
	struct snd_soc_jack jack;
	struct snd_soc_jack dp_jack[8];
	bool jack_setup;
	struct gpio_desc *speaker_earpiece;
	struct mutex speaker_earpiece_lock; /* Serialize the physical route control. */
};

static int sc8280xp_earpiece_get(struct snd_kcontrol *kcontrol,
				 struct snd_ctl_elem_value *value)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct sc8280xp_snd_data *data = snd_soc_card_get_drvdata(card);
	int state;

	guard(mutex)(&data->speaker_earpiece_lock);
	state = gpiod_get_value_cansleep(data->speaker_earpiece);
	if (state < 0)
		return state;
	value->value.integer.value[0] = state;
	return 0;
}

static int sc8280xp_earpiece_put(struct snd_kcontrol *kcontrol,
				 struct snd_ctl_elem_value *value)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct sc8280xp_snd_data *data = snd_soc_card_get_drvdata(card);
	long requested = value->value.integer.value[0];
	int state;

	if (requested != 0 && requested != 1)
		return -EINVAL;
	guard(mutex)(&data->speaker_earpiece_lock);
	state = gpiod_get_value_cansleep(data->speaker_earpiece);
	if (state < 0)
		return state;
	if (state == requested)
		return 0;
	state = gpiod_set_value_cansleep(data->speaker_earpiece, requested);
	return state ? state : 1;
}

static const struct snd_kcontrol_new sc8280xp_earpiece_control =
	SOC_SINGLE_BOOL_EXT("Speaker Earpiece Switch", 0,
			    sc8280xp_earpiece_get, sc8280xp_earpiece_put);

static int sc8280xp_snd_init(struct snd_soc_pcm_runtime *rtd)
{
	struct sc8280xp_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_soc_card *card = rtd->card;
	struct snd_soc_jack *dp_jack  = NULL;
	int dp_pcm_id = 0;

	switch (cpu_dai->id) {
	case PRIMARY_MI2S_RX...QUATERNARY_MI2S_TX:
	case QUINARY_MI2S_RX...QUINARY_MI2S_TX:
		snd_soc_dai_set_fmt(cpu_dai, SND_SOC_DAIFMT_BP_FP);
		break;
	case PRIMARY_TDM_RX_0...QUINARY_TDM_TX_7: {
		struct snd_soc_dai *codec_dai;
		unsigned int mask = GENMASK(SC8280XP_TDM_ACTIVE_SLOTS - 1, 0);
		int i;

		snd_soc_dai_set_fmt(cpu_dai, SND_SOC_DAIFMT_BP_FP |
				    data->tdm_fmt | SND_SOC_DAIFMT_NB_NF);
		snd_soc_dai_set_tdm_slot(cpu_dai, mask, mask, data->tdm_slots,
					 data->tdm_slot_width);
		for_each_rtd_codec_dais(rtd, i, codec_dai)
			snd_soc_dai_set_fmt(codec_dai, SND_SOC_DAIFMT_BC_FC |
					    SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF);
		return 0;
	}
	case WSA_CODEC_DMA_RX_0:
	case WSA_CODEC_DMA_RX_1:
		/*
		 * Set limit of -3 dB on Digital Volume and 0 dB on PA Volume
		 * to reduce the risk of speaker damage until we have active
		 * speaker protection in place.
		 */
		snd_soc_limit_volume(card, "WSA_RX0 Digital Volume", 81);
		snd_soc_limit_volume(card, "WSA_RX1 Digital Volume", 81);
		snd_soc_limit_volume(card, "SpkrLeft PA Volume", 17);
		snd_soc_limit_volume(card, "SpkrRight PA Volume", 17);
		break;
	case DISPLAY_PORT_RX_0:
		/* DISPLAY_PORT dai ids are not contiguous */
		dp_pcm_id = 0;
		dp_jack = &data->dp_jack[dp_pcm_id];
		break;
	case DISPLAY_PORT_RX_1 ... DISPLAY_PORT_RX_7:
		dp_pcm_id = cpu_dai->id - DISPLAY_PORT_RX_1 + 1;
		dp_jack = &data->dp_jack[dp_pcm_id];
		break;
	default:
		break;
	}

	if (dp_jack)
		return qcom_snd_dp_jack_setup(rtd, dp_jack, dp_pcm_id);

	return qcom_snd_wcd_jack_setup(rtd, &data->jack, &data->jack_setup);
}

static int sc8280xp_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
				     struct snd_pcm_hw_params *params)
{
	struct sc8280xp_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_interval *rate = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_CHANNELS);
	struct snd_mask *fmt = hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT);

	/* The marble headphone graph follows its frontend's negotiated format. */
	if (of_machine_is_compatible("xiaomi,marble") &&
	    cpu_dai->id == RX_CODEC_DMA_RX_0) {
		channels->min = channels->max = 2;
		return 0;
	}

	rate->min = rate->max = 48000;
	snd_mask_set_format(fmt, SNDRV_PCM_FORMAT_S16_LE);
	channels->min = 2;
	channels->max = 2;
	switch (cpu_dai->id) {
	case PRIMARY_TDM_RX_0...QUINARY_TDM_TX_7:
		rate->min = rate->max = data->tdm_rate;
		fallthrough;
	case PRIMARY_MI2S_RX...QUATERNARY_MI2S_TX:
	case QUINARY_MI2S_RX...QUINARY_MI2S_TX:
		/* Smart amps such as AW88261 expect 32-bit slots (BCLK = 64 * fs) */
		snd_mask_none(fmt);
		snd_mask_set_format(fmt, SNDRV_PCM_FORMAT_S32_LE);
		break;
	case TX_CODEC_DMA_TX_0:
	case TX_CODEC_DMA_TX_1:
	case TX_CODEC_DMA_TX_2:
	case TX_CODEC_DMA_TX_3:
		channels->min = 1;
		break;
	default:
		break;
	}


	return 0;
}

static int sc8280xp_snd_prepare(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct sc8280xp_snd_data *data = snd_soc_card_get_drvdata(rtd->card);

	return qcom_snd_sdw_prepare(substream, &data->stream_prepared[cpu_dai->id]);
}

static int sc8280xp_snd_hw_free(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct sc8280xp_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);

	return qcom_snd_sdw_hw_free(substream, &data->stream_prepared[cpu_dai->id]);
}

static bool sc8280xp_is_tdm(struct snd_soc_dai *cpu_dai)
{
	return cpu_dai->id >= PRIMARY_TDM_RX_0 && cpu_dai->id <= QUINARY_TDM_TX_7;
}

static int sc8280xp_snd_startup(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct sc8280xp_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	int ret;

	if (sc8280xp_is_tdm(cpu_dai)) {
		ret = clk_prepare_enable(data->tdm_vote);
		if (ret)
			return ret;

		/* The raw voice endpoint supplies its captured clock attributes. */
		if (!q6apm_voice_backend_link(rtd)) {
			ret = clk_set_rate(data->tdm_bclk,
					   data->tdm_rate * data->tdm_slots * data->tdm_slot_width);
			if (!ret)
				ret = clk_prepare_enable(data->tdm_bclk);
			if (ret) {
				clk_disable_unprepare(data->tdm_vote);
				return ret;
			}
		}
	}

	return qcom_snd_sdw_startup(substream);
}

static void sc8280xp_snd_shutdown(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct sc8280xp_snd_data *data = snd_soc_card_get_drvdata(rtd->card);

	qcom_snd_sdw_shutdown(substream);

	if (sc8280xp_is_tdm(cpu_dai)) {
		if (!q6apm_voice_backend_link(rtd))
			clk_disable_unprepare(data->tdm_bclk);
		clk_disable_unprepare(data->tdm_vote);
	}
}

static const struct snd_soc_ops sc8280xp_be_ops = {
	.startup = sc8280xp_snd_startup,
	.shutdown = sc8280xp_snd_shutdown,
	.hw_free = sc8280xp_snd_hw_free,
	.prepare = sc8280xp_snd_prepare,
};

static void sc8280xp_add_be_ops(struct snd_soc_card *card)
{
	struct snd_soc_dai_link *link;
	int i;

	for_each_card_prelinks(card, i, link) {
		if (link->no_pcm == 1) {
			link->init = sc8280xp_snd_init;
			link->be_hw_params_fixup = sc8280xp_be_hw_params_fixup;
			link->ops = &sc8280xp_be_ops;
		}
	}
}

static void sc8280xp_put_voice_node(void *node)
{
	of_node_put(node);
}

static int sc8280xp_add_voice_links(struct snd_soc_card *card)
{
	static const struct snd_soc_dapm_route voice_routes[] = {
		{ "AMIC5", NULL, "MIC BIAS3" },
	};
	struct snd_soc_dai_link *links, *rx = NULL, *tx = NULL;
	struct snd_soc_dai_link_component *platform;
	struct device_node *dais;
	int i, ret, count = card->num_links;

	if (!of_machine_is_compatible("xiaomi,marble"))
		return 0;
	for (i = 0; i < count; i++) {
		struct snd_soc_dai_link *link = &card->dai_link[i];

		if (link->no_pcm && link->id == TERTIARY_TDM_RX_0)
			rx = link;
		else if (link->no_pcm && link->id == TX_CODEC_DMA_TX_3)
			tx = link;
	}
	if (!rx || !tx)
		return -EINVAL;
	links = devm_kcalloc(card->dev, count + 2, sizeof(*links), GFP_KERNEL);
	platform = devm_kcalloc(card->dev, 2, sizeof(*platform), GFP_KERNEL);
	if (!links || !platform)
		return -ENOMEM;
	/* Frontends are loaded from topology, so no FE exists in the DT link array. */
	dais = of_get_compatible_child(rx->platforms[0].of_node, "qcom,q6apm-dais");
	if (!dais)
		return -ENODEV;
	ret = devm_add_action_or_reset(card->dev, sc8280xp_put_voice_node, dais);
	if (ret)
		return ret;
	memcpy(links, card->dai_link, count * sizeof(*links));
	for (i = 0; i < 2; i++) {
		struct snd_soc_dai_link *link = &links[count + i];

		*link = i ? *tx : *rx;
		platform[i].of_node = dais;
		link->platforms = &platform[i];
		link->num_platforms = 1;
		link->name = i ? Q6APM_VOICE_TX_LINK : Q6APM_VOICE_RX_LINK;
		link->stream_name = link->name;
		/* Leave topology PCM IDs 0..2 and BE base 16 untouched. */
		link->id = 14 + i;
		link->no_pcm = 0;
		link->dynamic = 0;
		link->playback_only = !i;
		link->capture_only = i;
		link->be_hw_params_fixup = NULL;
	}
	card->dai_link = links;
	card->num_links = count + 2;
	if (card->num_dapm_routes > 0 && card->dapm_routes) {
		struct snd_soc_dapm_route *new_routes;
		int new_count = card->num_dapm_routes + ARRAY_SIZE(voice_routes);

		new_routes = devm_kcalloc(card->dev, new_count, sizeof(*new_routes), GFP_KERNEL);
		if (!new_routes)
			return -ENOMEM;
		memcpy(new_routes, card->dapm_routes, card->num_dapm_routes * sizeof(*new_routes));
		memcpy(new_routes + card->num_dapm_routes, voice_routes, sizeof(voice_routes));
		card->dapm_routes = new_routes;
		card->num_dapm_routes = new_count;
	} else {
		card->dapm_routes = voice_routes;
		card->num_dapm_routes = ARRAY_SIZE(voice_routes);
	}
	return 0;
}

/* Speaker volume attenuates Music only; Receiver retains its own full range. */
static const DECLARE_TLV_DB_SCALE(sc8280xp_music_tlv, -8250, 25, 0);

static int sc8280xp_snd_late_probe(struct snd_soc_card *card)
{
	static const char * const names[] = {
		"Speaker Top Music Playback Volume",
		"Speaker Bottom Music Playback Volume",
	};
	struct snd_ctl_elem_value *value __free(kfree) = kzalloc_obj(*value);
	struct snd_kcontrol *followers[ARRAY_SIZE(names)], *receiver, *master;
	int i, ret;

	if (!of_machine_is_compatible("xiaomi,marble"))
		return 0;
	if (!value)
		return -ENOMEM;
	for (i = 0; i < ARRAY_SIZE(names); i++) {
		followers[i] = snd_ctl_find_id_mixer(card->snd_card, names[i]);
		if (!followers[i])
			return -ENODEV;
		ret = snd_soc_limit_volume(card, names[i], 330);
		if (ret < 0)
			return ret;
		value->value.integer.value[0] = 330;
		ret = followers[i]->put(followers[i], value);
		if (ret < 0)
			return ret;
	}
	receiver = snd_ctl_find_id_mixer(card->snd_card, "Speaker Top Receiver Playback Volume");
	if (!receiver)
		return -ENODEV;
	value->value.integer.value[0] = 360;
	ret = receiver->put(receiver, value);
	if (ret < 0)
		return ret;
	master = snd_ctl_make_virtual_master("Speaker Playback Volume", sc8280xp_music_tlv);
	if (!master)
		return -ENOMEM;
	for (i = 0; i < ARRAY_SIZE(names); i++) {
		ret = snd_ctl_add_follower(master, followers[i]);
		if (ret < 0) {
			snd_ctl_free_one(master);
			return ret;
		}
	}
	return snd_ctl_add(card->snd_card, master);
}

static int sc8280xp_platform_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card;
	struct sc8280xp_snd_data *data;
	struct device *dev = &pdev->dev;
	const char *tdm_fmt;
	int ret;

	card = devm_kzalloc(dev, sizeof(*card), GFP_KERNEL);
	if (!card)
		return -ENOMEM;
	card->owner = THIS_MODULE;
	/* Allocate the private data */
	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	mutex_init(&data->speaker_earpiece_lock);

	data->tdm_vote = devm_clk_get_optional(dev, "tdm-vote");
	if (IS_ERR(data->tdm_vote))
		return dev_err_probe(dev, PTR_ERR(data->tdm_vote), "failed to get tdm-vote clock\n");

	data->tdm_bclk = devm_clk_get_optional(dev, "tdm-bclk");
	if (IS_ERR(data->tdm_bclk))
		return dev_err_probe(dev, PTR_ERR(data->tdm_bclk), "failed to get tdm-bclk clock\n");

	data->speaker_earpiece = devm_gpiod_get_optional(dev, "speaker-earpiece", GPIOD_OUT_LOW);
	if (IS_ERR(data->speaker_earpiece))
		return dev_err_probe(dev, PTR_ERR(data->speaker_earpiece),
				     "failed to get speaker-earpiece GPIO\n");
	if (data->speaker_earpiece) {
		card->controls = &sc8280xp_earpiece_control;
		card->num_controls = 1;
	}

	data->tdm_slots = 2;
	data->tdm_slot_width = 32;
	data->tdm_rate = 48000;
	data->tdm_fmt = SND_SOC_DAIFMT_I2S;
	of_property_read_u32(dev->of_node, "qcom,tdm-slots", &data->tdm_slots);
	of_property_read_u32(dev->of_node, "qcom,tdm-slot-width", &data->tdm_slot_width);
	of_property_read_u32(dev->of_node, "qcom,tdm-rate", &data->tdm_rate);
	if (!of_property_read_string(dev->of_node, "qcom,tdm-format", &tdm_fmt)) {
		if (!strcmp(tdm_fmt, "dsp_a"))
			data->tdm_fmt = SND_SOC_DAIFMT_DSP_A;
		else if (!strcmp(tdm_fmt, "dsp_b"))
			data->tdm_fmt = SND_SOC_DAIFMT_DSP_B;
		else if (strcmp(tdm_fmt, "i2s"))
			return dev_err_probe(dev, -EINVAL, "invalid qcom,tdm-format\n");
	}

	card->dev = dev;
	dev_set_drvdata(dev, card);
	snd_soc_card_set_drvdata(card, data);
	ret = qcom_snd_parse_of(card);
	if (ret)
		return ret;

	card->driver_name = of_device_get_match_data(dev);
	card->late_probe = sc8280xp_snd_late_probe;
	sc8280xp_add_be_ops(card);
	ret = sc8280xp_add_voice_links(card);
	if (ret)
		return ret;
	return devm_snd_soc_register_card(dev, card);
}

static const struct of_device_id snd_sc8280xp_dt_match[] = {
	{.compatible = "qcom,eliza-sndcard", "eliza"},
	{.compatible = "qcom,kaanapali-sndcard", "kaanapali"},
	{.compatible = "qcom,qcm6490-idp-sndcard", "qcm6490"},
	{.compatible = "qcom,qcs615-sndcard", "qcs615"},
	{.compatible = "qcom,qcs6490-rb3gen2-sndcard", "qcs6490"},
	{.compatible = "qcom,qcs8275-sndcard", "qcs8300"},
	{.compatible = "qcom,qcs9075-sndcard", "sa8775p"},
	{.compatible = "qcom,qcs9100-sndcard", "sa8775p"},
	{.compatible = "qcom,sc8280xp-sndcard", "sc8280xp"},
	{.compatible = "qcom,sm8450-sndcard", "sm8450"},
	{.compatible = "qcom,sm8550-sndcard", "sm8550"},
	{.compatible = "qcom,sm8650-sndcard", "sm8650"},
	{.compatible = "qcom,sm8750-sndcard", "sm8750"},
	{}
};

MODULE_DEVICE_TABLE(of, snd_sc8280xp_dt_match);

static struct platform_driver snd_sc8280xp_driver = {
	.probe  = sc8280xp_platform_probe,
	.driver = {
		.name = "snd-sc8280xp",
		.of_match_table = snd_sc8280xp_dt_match,
	},
};
module_platform_driver(snd_sc8280xp_driver);
MODULE_AUTHOR("Srinivas Kandagatla <srinivas.kandagatla@linaro.org");
MODULE_DESCRIPTION("SC8280XP ASoC Machine Driver");
MODULE_LICENSE("GPL");
