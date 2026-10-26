// Flame and Night Sanctum markers (Flame Temples, Flame Shrines, Night Temples): position and the knowledge id(s) the
// game sets when the player first comes near (KnowledgeTriggerRange on the map label). From enshrouded.kfc; see
// tools/knowledge_tracking_notes.md "Flame / Night Sanctums". A sanctum is visited when one of its ids is known.
#pragma once
#include <cstdint>

struct SanctumSpot { float x, z; uint32_t id[2]; };
constexpr size_t kSanctumSpotCount = 93;
constexpr SanctumSpot kSanctumSpots[kSanctumSpotCount] = {
    {3676.5f, 3213.6f, {0xbf0321c3u, 0x00000000u}}, // Flame: deepforest_FlameTemple_01
    {5137.8f, 4186.2f, {0x3b836876u, 0x00000000u}}, // Flame: deepforest_FlameTemple_02
    {3168.5f, 4082.1f, {0x779e05dbu, 0x00000000u}}, // Flame: deepforest_FlameTemple_03
    {4900.4f, 3165.0f, {0xc0fcabddu, 0x00000000u}}, // Flame: deepforest_FlameTemple_04
    {4211.0f, 3879.6f, {0x449d24dau, 0x00000000u}}, // Flame: deepforest_FlameTemple_05
    {3235.7f, 1636.4f, {0x890545dcu, 0x00000000u}}, // Flame: general_FlameShrine_01_grassland
    {3392.8f, 2354.1f, {0xe44215cbu, 0x00000000u}}, // Flame: general_FlameShrine_01_grassland
    {3401.7f, 1856.4f, {0xf68d053au, 0x00000000u}}, // Flame: general_FlameShrine_01_grassland
    {3674.6f, 2208.2f, {0x2ffc07eeu, 0x00000000u}}, // Flame: general_FlameShrine_01_grassland
    {3907.3f, 1869.4f, {0xb2650308u, 0x00000000u}}, // Flame: general_FlameShrine_01_grassland
    {3937.1f, 1264.7f, {0x89d1d5a6u, 0x00000000u}}, // Flame: general_FlameShrine_01_grassland
    {4541.3f, 2041.6f, {0xcf407fcau, 0x00000000u}}, // Flame: general_FlameShrine_01_grassland
    {5126.1f, 1555.7f, {0x47d723dfu, 0x00000000u}}, // Flame: general_FlameShrine_01_grassland
    {3394.3f, 2979.6f, {0x1c431984u, 0x00000000u}}, // Flame: general_FlameShrine_02_deepforest
    {3486.1f, 4444.7f, {0x34fc33c5u, 0x00000000u}}, // Flame: general_FlameShrine_02_deepforest
    {3664.1f, 3556.2f, {0x300e442du, 0x00000000u}}, // Flame: general_FlameShrine_02_deepforest
    {3924.4f, 2896.3f, {0x73527f94u, 0x00000000u}}, // Flame: general_FlameShrine_02_deepforest
    {4434.6f, 3502.2f, {0x09da95aeu, 0x00000000u}}, // Flame: general_FlameShrine_02_deepforest
    {4908.7f, 3052.7f, {0x7c59d731u, 0x00000000u}}, // Flame: general_FlameShrine_02_deepforest
    {5125.3f, 3892.1f, {0x11c3611au, 0x00000000u}}, // Flame: general_FlameShrine_02_deepforest
    {5271.4f, 4183.8f, {0x4da37354u, 0x00000000u}}, // Flame: general_FlameShrine_02_deepforest_capital_lore
    {5400.8f, 2124.1f, {0x36f67974u, 0x00000000u}}, // Flame: general_FlameShrine_03_steppes
    {5690.3f, 3231.1f, {0x6f213330u, 0x00000000u}}, // Flame: general_FlameShrine_03_steppes
    {5740.1f, 2371.9f, {0x4ab92671u, 0x00000000u}}, // Flame: general_FlameShrine_03_steppes
    {5998.3f, 3841.6f, {0xf5d8b339u, 0x00000000u}}, // Flame: general_FlameShrine_03_steppes
    {6036.5f, 3072.2f, {0x81033c8cu, 0x00000000u}}, // Flame: general_FlameShrine_03_steppes
    {6083.1f, 2765.7f, {0x4d7f7681u, 0x00000000u}}, // Flame: general_FlameShrine_03_steppes
    {6183.6f, 2074.9f, {0x63090f46u, 0x00000000u}}, // Flame: general_FlameShrine_03_steppes
    {6771.4f, 4074.0f, {0xb1930163u, 0x00000000u}}, // Flame: general_FlameShrine_03_steppes
    {7261.6f, 3543.5f, {0x05e2b886u, 0x00000000u}}, // Flame: general_FlameShrine_04_desert
    {7455.6f, 2645.5f, {0xb5331187u, 0x00000000u}}, // Flame: general_FlameShrine_04_desert
    {7987.0f, 3944.9f, {0x8ce6b930u, 0x00000000u}}, // Flame: general_FlameShrine_04_desert
    {8278.6f, 2624.5f, {0xb7927e02u, 0x00000000u}}, // Flame: general_FlameShrine_04_desert
    {8441.5f, 2863.1f, {0x2e80d12bu, 0x00000000u}}, // Flame: general_FlameShrine_04_desert
    {8663.6f, 3912.3f, {0xc84d505bu, 0x00000000u}}, // Flame: general_FlameShrine_04_desert
    {8988.0f, 3350.4f, {0x23daf14fu, 0x00000000u}}, // Flame: general_FlameShrine_04_desert
    {6506.9f, 6241.6f, {0xcd7b6eedu, 0x00000000u}}, // Flame: general_FlameShrine_05_coldheights
    {7018.8f, 5633.7f, {0x1e2ef73eu, 0x00000000u}}, // Flame: general_FlameShrine_05_coldheights
    {7561.0f, 5261.7f, {0xff9370bcu, 0x00000000u}}, // Flame: general_FlameShrine_05_coldheights
    {7641.5f, 4295.8f, {0x37a0394fu, 0x00000000u}}, // Flame: general_FlameShrine_05_coldheights
    {7801.5f, 5863.0f, {0x2f70a703u, 0x00000000u}}, // Flame: general_FlameShrine_05_coldheights
    {7862.6f, 6400.8f, {0xec1dca03u, 0x00000000u}}, // Flame: general_FlameShrine_05_coldheights
    {8654.4f, 4631.8f, {0xde4f0531u, 0x00000000u}}, // Flame: general_FlameShrine_05_coldheights
    {8674.5f, 4943.8f, {0xe4a2f23au, 0x00000000u}}, // Flame: general_FlameShrine_05_coldheights
    {3895.6f, 6139.8f, {0x26ff2d99u, 0x00000000u}}, // Flame: general_FlameShrine_06_wetland
    {3897.3f, 7156.7f, {0xfe7bb567u, 0x00000000u}}, // Flame: general_FlameShrine_06_wetland
    {4250.7f, 5141.7f, {0x11ed1319u, 0x00000000u}}, // Flame: general_FlameShrine_06_wetland
    {4723.4f, 6444.2f, {0x790fb170u, 0x00000000u}}, // Flame: general_FlameShrine_06_wetland
    {4950.8f, 5356.2f, {0xad67c5a0u, 0x00000000u}}, // Flame: general_FlameShrine_06_wetland
    {5313.3f, 5944.8f, {0xc4301e12u, 0x00000000u}}, // Flame: general_FlameShrine_06_wetland
    {5785.7f, 4588.8f, {0x7e8b4d79u, 0x00000000u}}, // Flame: general_FlameShrine_06_wetland
    {6144.3f, 5469.4f, {0xbacf229cu, 0x00000000u}}, // Flame: general_FlameShrine_06_wetland
    {7963.0f, 1118.9f, {0x53fcc8d8u, 0x00000000u}}, // Flame: general_FlameShrine_lore_desert_ministory_22_hiddenFlameShrine
    {2932.5f, 1462.6f, {0x219a82ebu, 0x00000000u}}, // Flame: grassland_FlameTemple_01_NurseHelen
    {3002.1f, 2857.0f, {0xe8833977u, 0x00000000u}}, // Flame: grassland_FlameTemple_02
    {5122.0f, 2253.9f, {0xfa463dbfu, 0x00000000u}}, // Flame: grassland_FlameTemple_03
    {4199.4f, 2506.0f, {0xa9b3d928u, 0x00000000u}}, // Flame: grassland_FlameTemple_04
    {4810.4f, 1341.6f, {0x45786101u, 0x00000000u}}, // Flame: grassland_FlameTemple_05
    {5809.6f, 4318.5f, {0xbd6a9d20u, 0x00000000u}}, // Flame: steppes_FlameTemple_01
    {6723.0f, 2332.6f, {0x5c3abca5u, 0x00000000u}}, // Flame: steppes_FlameTemple_02
    {6796.8f, 2869.4f, {0x0cd18063u, 0x00000000u}}, // Flame: steppes_FlameTemple_03
    {5435.9f, 2562.2f, {0x59a60083u, 0x00000000u}}, // Flame: steppes_FlameTemple_04
    {5624.2f, 3057.6f, {0x74f4b809u, 0x00000000u}}, // Flame: steppes_FlameTemple_05
    {4616.0f, 2780.3f, {0x5e45143cu, 0x00000000u}}, // Night: AncientDungeon_06_Barber
    {8431.8f, 6776.4f, {0xb0b433f8u, 0x00000000u}}, // Night: AncientDungeon_NightTemple_coldHeights_gemstation
    {4709.3f, 4109.8f, {0xef18ee59u, 0x00000000u}}, // Night: AncientDungeon_NightTemple_deepforest_gemstation
    {4356.7f, 3352.3f, {0x42bcaa1du, 0x91e1e213u}}, // Night: AncientDungeon_NightTemple_deepforest_logicTool
    {9049.2f, 1717.8f, {0x13917753u, 0x00000000u}}, // Night: AncientDungeon_NightTemple_desert_gemstation
    {2479.2f, 4845.7f, {0x97110924u, 0x00000000u}}, // Night: AncientDungeon_NightTemple_general
    {2797.3f, 3032.3f, {0x112fad32u, 0x00000000u}}, // Night: AncientDungeon_NightTemple_general
    {3001.3f, 2337.3f, {0x3d8f8b9bu, 0x00000000u}}, // Night: AncientDungeon_NightTemple_general
    {3899.3f, 4266.1f, {0xb40bee33u, 0x00000000u}}, // Night: AncientDungeon_NightTemple_general
    {4126.8f, 4789.3f, {0x012c7cb7u, 0x00000000u}}, // Night: AncientDungeon_NightTemple_general
    {4205.3f, 1768.3f, {0x566523aeu, 0x00000000u}}, // Night: AncientDungeon_NightTemple_general
    {4363.3f, 2467.3f, {0xa29c7303u, 0x00000000u}}, // Night: AncientDungeon_NightTemple_general
    {5126.8f, 5032.7f, {0x3b180145u, 0x00000000u}}, // Night: AncientDungeon_NightTemple_general
    {5351.6f, 2400.3f, {0xf37c0008u, 0x00000000u}}, // Night: AncientDungeon_NightTemple_general
    {5978.3f, 2409.3f, {0x5c5b0bd0u, 0x00000000u}}, // Night: AncientDungeon_NightTemple_general
    {6081.3f, 3706.6f, {0xbbcf6a3bu, 0x00000000u}}, // Night: AncientDungeon_NightTemple_general
    {6260.2f, 4955.2f, {0x8fb4ee11u, 0x00000000u}}, // Night: AncientDungeon_NightTemple_general
    {6756.3f, 1199.3f, {0x2838336eu, 0x00000000u}}, // Night: AncientDungeon_NightTemple_general
    {6933.3f, 4098.7f, {0x13c47a63u, 0x00000000u}}, // Night: AncientDungeon_NightTemple_general
    {7326.2f, 5978.7f, {0xa1d0d2b1u, 0x00000000u}}, // Night: AncientDungeon_NightTemple_general
    {7487.3f, 3604.3f, {0x9a2375c1u, 0x00000000u}}, // Night: AncientDungeon_NightTemple_general
    {7556.7f, 4408.4f, {0x49e4ead2u, 0x00000000u}}, // Night: AncientDungeon_NightTemple_general
    {7670.3f, 2914.3f, {0x3d53ee0cu, 0x00000000u}}, // Night: AncientDungeon_NightTemple_general
    {7793.7f, 6564.3f, {0xdbc39de5u, 0x00000000u}}, // Night: AncientDungeon_NightTemple_general
    {8277.3f, 4750.3f, {0x80deda2cu, 0x00000000u}}, // Night: AncientDungeon_NightTemple_general
    {9091.7f, 3308.7f, {0x28520208u, 0x00000000u}}, // Night: AncientDungeon_NightTemple_general
    {5446.3f, 1184.7f, {0x6d5ebe91u, 0x00000000u}}, // Night: AncientDungeon_NightTemple_grassland_gemstation
    {6650.7f, 2575.3f, {0x2e11cf1eu, 0x00000000u}}, // Night: AncientDungeon_NightTemple_steppes_gemstation
    {5396.5f, 6902.2f, {0xa3d23c43u, 0x00000000u}}, // Night: AncientDungeon_NightTemple_wetlands_gemstation
    {5726.2f, 6310.8f, {0x67af6ebfu, 0x00000000u}}, // Night: AncientDungeon_NightTemple_wetlands_lycanthropyGem
};
