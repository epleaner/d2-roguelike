"""Pinned source identities and semantic S14 map edits; never contains game bytes.

Derived from player-owned authenticated MPQs; every output byte is checked against
the immutable Store record before staging. Do not substitute an unpinned archive.
"""
import json

DATA = json.loads(r'''
{
  "archives": {
    "d2data.mpq": "e70adad3aa23f1f72ecd53f01a2cfd9ae1aa7ac043549cb4c881c2d30a47a1af",
    "d2exp.mpq": "04e35bffeae8d9959042832055702f8b5b4567ca5505ff1df0cf3fd6d5beb6c2",
    "patch_d2.mpq": "b976e7847dda696f54198a9477819f96579c4894f54381c72afb4937e919e0a9",
    "pd2maps.mpq": "e42428e61f67bfdcbe180032bd2864d8fbacf61067c88e79ec69a4fe1fb60061"
  },
  "recipes": {
    "data/global/tiles/Act1/Barracks/RogC-barracks.ds1": {
      "archive": "d2data.mpq",
      "member": "data/global/tiles/act1/barracks/barewtheme1.ds1",
      "sourceSha": "7d9fa90746dcad1715fbfd0d89d1786ea58249c4165e8a6e7773f5fb81c0cae7",
      "output": {
        "bytes": 2823,
        "path": "data/global/tiles/Act1/Barracks/RogC-barracks.ds1",
        "sha256": "9c8a516abad7231b956f72492fe8742a5e82657f805e40da341a27c6d9bc6c4b"
      },
      "sourceBytes": 3123,
      "operation": "clear",
      "walls": []
    },
    "data/global/tiles/Act1/Catacomb/RogC-andyroom.ds1": {
      "archive": "d2data.mpq",
      "member": "data/global/tiles/act1/catacomb/andy3.ds1",
      "sourceSha": "92f9911ba27e813aa319b6c9c7e6cddcb340406a67ce24bd00d4971553721639",
      "output": {
        "bytes": 12007,
        "path": "data/global/tiles/Act1/Catacomb/RogC-andyroom.ds1",
        "sha256": "ce39a34f5cac0618c4bdea463ed4b0b0b8290ecc4e742a23baa680b6776a467e"
      },
      "sourceBytes": 14007,
      "operation": "clear",
      "walls": [
        [
          0,
          18,
          22
        ],
        [
          0,
          19,
          22
        ]
      ]
    },
    "data/global/tiles/Act1/Catacomb/RogC-catacomb.ds1": {
      "archive": "d2data.mpq",
      "member": "data/global/tiles/act1/catacomb/catnsewtheme1.ds1",
      "sourceSha": "c2dd9b892aed9ac4bc592526b7bf15b6055deb43df39db6d92e11ac3a4e892e2",
      "output": {
        "bytes": 4192,
        "path": "data/global/tiles/Act1/Catacomb/RogC-catacomb.ds1",
        "sha256": "42abbf4b494db866b56c1a6c7409e1894c7c016a2fab1f74be8b0cbf983d5c94"
      },
      "sourceBytes": 4332,
      "operation": "clear",
      "walls": []
    },
    "data/global/tiles/Act1/Cathedrl/RogCathy.ds1": {
      "archive": "d2data.mpq",
      "member": "data/global/tiles/act1/cathedrl/cathy3.ds1",
      "sourceSha": "5c4c33f0597b3fa1b86a9c77d9158b37fccc63c3943a2306753e67b2faf8cc60",
      "output": {
        "bytes": 16746,
        "path": "data/global/tiles/Act1/Cathedrl/RogCathy.ds1",
        "sha256": "ddcc4972176615893f0f18619b00c44c0972f48b80f097601e03aec138d8380b"
      },
      "sourceBytes": 17346,
      "operation": "clear",
      "walls": [
        [
          0,
          21,
          10
        ],
        [
          0,
          22,
          10
        ]
      ]
    },
    "data/global/tiles/Act1/Caves/RogC-cave.ds1": {
      "archive": "d2data.mpq",
      "member": "data/global/tiles/act1/caves/caveroom4.ds1",
      "sourceSha": "22617de37f8679c780857224d319d10d335aede633ca9f8d7504e9845957b9b4",
      "output": {
        "bytes": 10127,
        "path": "data/global/tiles/Act1/Caves/RogC-cave.ds1",
        "sha256": "402356b4a9443ef05482e9c25870056d17ee68a1905519d7e170220261d2984e"
      },
      "sourceBytes": 10367,
      "operation": "clear",
      "walls": [
        [
          0,
          18,
          9
        ]
      ]
    },
    "data/global/tiles/Act1/Caves/RogC-cavefive.ds1": {
      "archive": "d2data.mpq",
      "member": "data/global/tiles/act1/caves/caveroom5.ds1",
      "sourceSha": "b963c83e6fea9a84c65d7e3b71f922cf51d87fb588b8eeeea6b06a9abb44289e",
      "output": {
        "bytes": 10127,
        "path": "data/global/tiles/Act1/Caves/RogC-cavefive.ds1",
        "sha256": "56590238708c57a8fb9fd7a15d63fae912425904b7cea1c796f5297fd00d21f3"
      },
      "sourceBytes": 10467,
      "operation": "clear",
      "walls": [
        [
          0,
          4,
          5
        ]
      ]
    },
    "data/global/tiles/Act1/Caves/RogC-cavetwo.ds1": {
      "archive": "d2data.mpq",
      "member": "data/global/tiles/act1/caves/caveroom2.ds1",
      "sourceSha": "bd2e1687fddedf244af777d745876d7c63c34791ea3241e88af262cc288b7305",
      "output": {
        "bytes": 10127,
        "path": "data/global/tiles/Act1/Caves/RogC-cavetwo.ds1",
        "sha256": "46e9d2229efc7a647b9ca92e6a62d3c72ec63a084880bc86cc41b03e47649ab9"
      },
      "sourceBytes": 10467,
      "operation": "clear",
      "walls": [
        [
          0,
          4,
          5
        ]
      ]
    },
    "data/global/tiles/Act1/Court/RogC-courtyard.ds1": {
      "archive": "d2exp.mpq",
      "member": "data/global/tiles/act1/court/cat_court.ds1",
      "sourceSha": "07f23f24b3c47221467727781b0e60fa8ea6e53d512a2d12bd0bf5cb8f978984",
      "output": {
        "bytes": 6651,
        "path": "data/global/tiles/Act1/Court/RogC-courtyard.ds1",
        "sha256": "27ec99b12831ec7d1464645a4f6416d2a32195c3b4d76de09f840aad6bb689e1"
      },
      "sourceBytes": 6711,
      "operation": "clear",
      "walls": [
        [
          0,
          2,
          3
        ]
      ]
    },
    "data/global/tiles/Act1/Crypt/RogC-countess.ds1": {
      "archive": "d2data.mpq",
      "member": "data/global/tiles/act1/crypt/cryptcountess1.ds1",
      "sourceSha": "545f6bdda1a2e233a6e9a2ce8769387f99486ac6ae42127d233cb9d9b3eb100e",
      "output": {
        "bytes": 27114,
        "path": "data/global/tiles/Act1/Crypt/RogC-countess.ds1",
        "sha256": "9944591ac7778a51ccdaa99a6634c6c269b3b77bcb9e620152626d49e74e0488"
      },
      "sourceBytes": 28134,
      "operation": "clear",
      "walls": [
        [
          0,
          26,
          12
        ],
        [
          0,
          27,
          12
        ]
      ]
    },
    "data/global/tiles/Act1/Crypt/RogC-cryptalt.ds1": {
      "archive": "d2data.mpq",
      "member": "data/global/tiles/act1/crypt/cryptcountess2.ds1",
      "sourceSha": "82367a0aa75e983f22282e70e22a6b1cbc861c9210821005a8e31a43b93d1a31",
      "output": {
        "bytes": 27114,
        "path": "data/global/tiles/Act1/Crypt/RogC-cryptalt.ds1",
        "sha256": "5096fef91ab1bcf0564d31793f3b53678d2037a1572a57faea6bfaf052dd834b"
      },
      "sourceBytes": 28054,
      "operation": "clear",
      "walls": [
        [
          0,
          13,
          25
        ],
        [
          0,
          14,
          25
        ]
      ]
    },
    "data/global/tiles/Act1/Graveyard/RogC-graveyard.ds1": {
      "archive": "d2data.mpq",
      "member": "data/global/tiles/act1/graveyard/gravey.ds1",
      "sourceSha": "ea3199bbc5913394dbea549f62bd37c6f776c9dc69ef4a0881150860b08b4ccf",
      "output": {
        "bytes": 20075,
        "path": "data/global/tiles/Act1/Graveyard/RogC-graveyard.ds1",
        "sha256": "ff607b5c44dbf7b0a351a54458a8d7b2c870b8b319f5f3c91408b268035fa3b2"
      },
      "sourceBytes": 20295,
      "operation": "clear",
      "walls": [
        [
          0,
          11,
          6
        ],
        [
          1,
          12,
          27
        ]
      ]
    },
    "data/global/tiles/Act1/Town/TownE1.ds1": {
      "archive": "d2data.mpq",
      "member": "data/global/tiles/act1/town/towne1.ds1",
      "sourceSha": "cbbe393404bad4d51110bf551b3cc2987f74bfc89d5eb2bbec8d1521545b1685",
      "sourceBytes": 57691,
      "output": {
        "bytes": 56983,
        "path": "data/global/tiles/Act1/Town/TownE1.ds1",
        "sha256": "8fca7caf0c680a310af1e34797bc8e9ef9370e8890a383a00faaab46c1338027"
      },
      "operation": "town",
      "walls": []
    },
    "data/global/tiles/Act1/Town/TownN1.ds1": {
      "archive": "d2data.mpq",
      "member": "data/global/tiles/act1/town/townn1.ds1",
      "sourceSha": "d4b39b1b4fefa6c7099bd0caf82e7464be0ea5f6ab8641dfbb20c19c34663654",
      "sourceBytes": 57652,
      "output": {
        "bytes": 56956,
        "path": "data/global/tiles/Act1/Town/TownN1.ds1",
        "sha256": "52e69419b682eadc147c5a2fadad950fe931659c70b31242c57c9e11d786b90f"
      },
      "operation": "town",
      "walls": []
    },
    "data/global/tiles/Act1/Town/TownS1.ds1": {
      "archive": "d2data.mpq",
      "member": "data/global/tiles/act1/town/towns1.ds1",
      "sourceSha": "dc0e15b9014655a90a292ecda7e2d0b34ca58a0f870f66f54a3898fcd6f6d184",
      "sourceBytes": 57672,
      "output": {
        "bytes": 56956,
        "path": "data/global/tiles/Act1/Town/TownS1.ds1",
        "sha256": "200f484d8897277e1b4a5a6c7c5ec92eb7a2e98c2af84187baef7a615c59f2ae"
      },
      "operation": "town",
      "walls": []
    },
    "data/global/tiles/Act1/Town/TownW1.ds1": {
      "archive": "d2data.mpq",
      "member": "data/global/tiles/act1/town/townw1.ds1",
      "sourceSha": "f4648bab6078c094ccb84e47d4788824e66cab1075edfe1339d9b6161d092692",
      "sourceBytes": 57700,
      "output": {
        "bytes": 56956,
        "path": "data/global/tiles/Act1/Town/TownW1.ds1",
        "sha256": "72b9fb0bd5e29ad0ac7c9bfa56aa0e8bee5edc4d4c91e1838064339b9980ab19"
      },
      "operation": "town",
      "walls": []
    },
    "data/global/tiles/Act2/Arcane/RogC-summoner.ds1": {
      "archive": "pd2maps.mpq",
      "member": "data/global/tiles/act2/arcane/summw.ds1",
      "sourceSha": "435ca0434406bad7ec183f8c36e3ab4919479c3c9f192318ecc971b14548cd71",
      "output": {
        "bytes": 4301,
        "path": "data/global/tiles/Act2/Arcane/RogC-summoner.ds1",
        "sha256": "53a9ef9603bc835acc82a43840c414d19775569691fe8f290320e3a1af97ab51"
      },
      "sourceBytes": 4461,
      "operation": "clear",
      "walls": []
    },
    "data/global/tiles/Act2/Maggot/RogC-queen.ds1": {
      "archive": "pd2maps.mpq",
      "member": "data/global/tiles/act2/maggot/magqueen.ds1",
      "sourceSha": "867339e803f2ef14e25f5673b928fe07c8db5e9b5e1cd7da7973347b5f234b8b",
      "output": {
        "bytes": 2219,
        "path": "data/global/tiles/Act2/Maggot/RogC-queen.ds1",
        "sha256": "322514ff16d277db33bfda922b0a0f1fba1088f973f0d401dd66e473144b72a2"
      },
      "sourceBytes": 2259,
      "operation": "clear",
      "walls": []
    },
    "data/global/tiles/Act2/Palace/Harem2.ds1": {
      "archive": "d2data.mpq",
      "member": "data/global/tiles/act2/palace/harem2.ds1",
      "sourceSha": "a4104e7ccb81d4fccf7d9c864993e1e6fe317e9905d597f898da254e9b41dfa7",
      "sourceBytes": 8692,
      "output": {
        "bytes": 8692,
        "path": "data/global/tiles/Act2/Palace/Harem2.ds1",
        "sha256": "a4104e7ccb81d4fccf7d9c864993e1e6fe317e9905d597f898da254e9b41dfa7"
      },
      "operation": "copy",
      "walls": []
    },
    "data/global/tiles/Act2/Palace/RogC-palace.ds1": {
      "archive": "d2data.mpq",
      "member": "data/global/tiles/act2/palace/celne.ds1",
      "sourceSha": "4d60924ed657037a48c99017f95ad4563255a452ceff2973964465dbd2f9d02b",
      "output": {
        "bytes": 7159,
        "path": "data/global/tiles/Act2/Palace/RogC-palace.ds1",
        "sha256": "1f3f8b5233cc6fdf4101e959b5a73e30aa5ca69f35eb3ef5b959cd4716c2ce4b"
      },
      "sourceBytes": 7379,
      "operation": "clear",
      "walls": [
        [
          0,
          0,
          14
        ]
      ]
    },
    "data/global/tiles/Act2/Palace/RogHrm.ds1": {
      "archive": "d2data.mpq",
      "member": "data/global/tiles/act2/palace/harem2.ds1",
      "sourceSha": "a4104e7ccb81d4fccf7d9c864993e1e6fe317e9905d597f898da254e9b41dfa7",
      "sourceBytes": 8692,
      "output": {
        "bytes": 8692,
        "path": "data/global/tiles/Act2/Palace/RogHrm.ds1",
        "sha256": "d61c2a28788440dbfb317ba7db863a9e76cb6dad5530e178aaf87d02001796d2"
      },
      "operation": "clear_walls",
      "walls": [
        [
          0,
          9,
          3
        ],
        [
          0,
          9,
          17
        ],
        [
          1,
          1,
          9
        ]
      ]
    },
    "data/global/tiles/Act2/Tomb/RogC-tombtheme.ds1": {
      "archive": "d2data.mpq",
      "member": "data/global/tiles/act2/tomb/tombewtheme.ds1",
      "sourceSha": "dd0cac12d21c5b9a1cce9461340c3f23b3e140f98b93a6540d02a370ec052987",
      "output": {
        "bytes": 4749,
        "path": "data/global/tiles/Act2/Tomb/RogC-tombtheme.ds1",
        "sha256": "80d55a169bbd736bbb71cd2704636a5ff633d999ebf8062b4661a6354e26d14a"
      },
      "sourceBytes": 4929,
      "operation": "clear",
      "walls": []
    },
    "data/global/tiles/Act2/Tomb/RogTombE.ds1": {
      "archive": "d2data.mpq",
      "member": "data/global/tiles/act2/tomb/tombetreasure.ds1",
      "sourceSha": "b1638f88c214b46ec4b4ff797c940a136b79f214b80bf0c9e960c8a80c3848f4",
      "output": {
        "bytes": 10530,
        "path": "data/global/tiles/Act2/Tomb/RogTombE.ds1",
        "sha256": "e4fd105db0870d7d4331b3771f34042b61e57db86ddee04b0b1550906831ac5f"
      },
      "sourceBytes": 10890,
      "operation": "clear",
      "walls": []
    },
    "data/global/tiles/Act3/Sewer/RogC-sewer.ds1": {
      "archive": "d2data.mpq",
      "member": "data/global/tiles/act3/sewer/sewertreasure1.ds1",
      "sourceSha": "01d744de8febcd263a2de6f3d50639769120ea45e632955248494d4803ff9f7c",
      "output": {
        "bytes": 7209,
        "path": "data/global/tiles/Act3/Sewer/RogC-sewer.ds1",
        "sha256": "8bf6d03e2b077dba2ceb9cb29364aaed05c5568932edffd11e149d7c3d802d29"
      },
      "sourceBytes": 7669,
      "operation": "clear",
      "walls": [
        [
          0,
          1,
          10
        ],
        [
          0,
          1,
          11
        ]
      ]
    },
    "data/global/tiles/Act3/Sewer/RogC-sewerchest.ds1": {
      "archive": "d2data.mpq",
      "member": "data/global/tiles/act3/sewer/sewerntreas.ds1",
      "sourceSha": "df4c06f03305a8f3ccd6aaf5109b0ac951adce0c353aed1dca3668751bf6d4cd",
      "output": {
        "bytes": 4277,
        "path": "data/global/tiles/Act3/Sewer/RogC-sewerchest.ds1",
        "sha256": "7f699406aede2e0a50f3c083eff9a4b813fe378b1f169f8af4e54dc1ff39bd33"
      },
      "sourceBytes": 4317,
      "operation": "clear",
      "walls": []
    },
    "data/global/tiles/Act3/Temple/RogC-temple.ds1": {
      "archive": "d2data.mpq",
      "member": "data/global/tiles/act3/temple/temple4.ds1",
      "sourceSha": "da7c03cf568963a1aa6e7705fbda9638600668a09f4b73f73a79fec9f92d5ef3",
      "output": {
        "bytes": 15180,
        "path": "data/global/tiles/Act3/Temple/RogC-temple.ds1",
        "sha256": "4511115ec01fe4f51dac4386f90d2e07eb5dc3df8b497904a2fd520aa0928b62"
      },
      "sourceBytes": 15600,
      "operation": "clear",
      "walls": [
        [
          0,
          11,
          4
        ]
      ]
    },
    "data/global/tiles/Act3/Temple/RogC-templefifth.ds1": {
      "archive": "d2exp.mpq",
      "member": "data/global/tiles/act3/temple/temple10.ds1",
      "sourceSha": "c068193e2a293f161d25077e5caf100af64bf9526ad3aadd281a32e081ef06ba",
      "output": {
        "bytes": 15180,
        "path": "data/global/tiles/Act3/Temple/RogC-templefifth.ds1",
        "sha256": "ab40220fb95ca70efa9503aa2c5d0b92b5000530bae9b8ffeaf1e52782a30a40"
      },
      "sourceBytes": 15520,
      "operation": "clear",
      "walls": [
        [
          0,
          0,
          9
        ]
      ]
    },
    "data/global/tiles/Act3/Temple/RogC-templefour.ds1": {
      "archive": "d2data.mpq",
      "member": "data/global/tiles/act3/temple/temple7.ds1",
      "sourceSha": "d6137b5cb2f9adf286bffcf3c2520ba0a8308425b9542bf28bd9ce4c7c795b2a",
      "output": {
        "bytes": 15180,
        "path": "data/global/tiles/Act3/Temple/RogC-templefour.ds1",
        "sha256": "f87e2663de6763994450854dea575d9618465f547b7eb4882b7f983d5c5f0b34"
      },
      "sourceBytes": 15960,
      "operation": "clear",
      "walls": [
        [
          0,
          12,
          19
        ]
      ]
    },
    "data/global/tiles/Act3/Temple/RogC-templeone.ds1": {
      "archive": "d2data.mpq",
      "member": "data/global/tiles/act3/temple/temple0.ds1",
      "sourceSha": "0a3d6e228bef54f2376e1b3b23087fba1e4ff42203e2db3019811b6d9986d375",
      "output": {
        "bytes": 20229,
        "path": "data/global/tiles/Act3/Temple/RogC-templeone.ds1",
        "sha256": "9b3f33d61991a7a5e64aab17a1205bccabc9c3ca3741cd63b24726c75f85bfd4"
      },
      "sourceBytes": 20549,
      "operation": "clear",
      "walls": [
        [
          0,
          0,
          11
        ]
      ]
    },
    "data/global/tiles/Act3/Temple/RogC-templesixth.ds1": {
      "archive": "d2data.mpq",
      "member": "data/global/tiles/act3/temple/temple12.ds1",
      "sourceSha": "2ef30433900c04d98fa912ca1bd395e21a6e4acfa3b2751ee27154b36d6b3292",
      "output": {
        "bytes": 20180,
        "path": "data/global/tiles/Act3/Temple/RogC-templesixth.ds1",
        "sha256": "8d132a8c9f4ef7569c96d5784a6f99ccba46bbd8bb02a77221b8e0a40207fdc0"
      },
      "sourceBytes": 21060,
      "operation": "clear",
      "walls": [
        [
          0,
          10,
          0
        ]
      ]
    },
    "data/global/tiles/Act3/Temple/RogC-templethird.ds1": {
      "archive": "d2data.mpq",
      "member": "data/global/tiles/act3/temple/temple6.ds1",
      "sourceSha": "4f101ce1e642a2a45528536b7e0322d1470cc7aabf01adec72221d015fc2ced0",
      "output": {
        "bytes": 15180,
        "path": "data/global/tiles/Act3/Temple/RogC-templethird.ds1",
        "sha256": "5ed33aef6753b5860e96e53a14ccea0b178bd242af3d06cf202b044539e19a70"
      },
      "sourceBytes": 15780,
      "operation": "clear",
      "walls": [
        [
          0,
          11,
          1
        ]
      ]
    },
    "data/global/tiles/Act3/Temple/RogTemple.ds1": {
      "archive": "d2data.mpq",
      "member": "data/global/tiles/act3/temple/temple3.ds1",
      "sourceSha": "9189babd495ce2edf32465490ffb762636583239e62dd68849dd2b567d944b2c",
      "output": {
        "bytes": 15180,
        "path": "data/global/tiles/Act3/Temple/RogTemple.ds1",
        "sha256": "03f8cc41c55239903620270354badcf93c3ea4edae637ed4d70e64f0273193be"
      },
      "sourceBytes": 15640,
      "operation": "clear",
      "walls": [
        [
          0,
          11,
          2
        ]
      ]
    },
    "data/global/tiles/Act3/Travincal/RogC-durance.ds1": {
      "archive": "d2data.mpq",
      "member": "data/global/tiles/act3/travincal/mephnetheme.ds1",
      "sourceSha": "15cbdb111b45661e9d6ca1b654de83b13446e2f1ba703c9df2cc0ac62e2df389",
      "output": {
        "bytes": 4335,
        "path": "data/global/tiles/Act3/Travincal/RogC-durance.ds1",
        "sha256": "ffc2fa37a4d149ea1ba0647374a91fae38f6c44b8a9efcc9e3a52fefc4989b17"
      },
      "sourceBytes": 4515,
      "operation": "clear",
      "walls": []
    },
    "data/global/tiles/Act4/Diab/RogC-chaose.ds1": {
      "archive": "patch_d2.mpq",
      "member": "data/global/tiles/act4/diab/winge1.ds1",
      "sourceSha": "714cacb2443a49dc8504a93e8344d5695a352904cb53889e67aa13ac9c794bd7",
      "output": {
        "bytes": 22751,
        "path": "data/global/tiles/Act4/Diab/RogC-chaose.ds1",
        "sha256": "82f18e7de979e4aedc932199e3ce23c829975e2bb9424aa6f568520138e9160d"
      },
      "sourceBytes": 23191,
      "operation": "clear",
      "walls": []
    },
    "data/global/tiles/Act4/Diab/RogC-chaosn.ds1": {
      "archive": "patch_d2.mpq",
      "member": "data/global/tiles/act4/diab/wingn1.ds1",
      "sourceSha": "38c657bb6fa413d424720c69621370df0c6d29ddcbf6510b1b772aa6a822a75f",
      "output": {
        "bytes": 22751,
        "path": "data/global/tiles/Act4/Diab/RogC-chaosn.ds1",
        "sha256": "c9520e130612bc42a768c0e9c80ab8c9567d76cfd218db93c64d46babc0c38d4"
      },
      "sourceBytes": 23191,
      "operation": "clear",
      "walls": []
    },
    "data/global/tiles/Act4/Diab/RogC-chaosw.ds1": {
      "archive": "patch_d2.mpq",
      "member": "data/global/tiles/act4/diab/wingw1.ds1",
      "sourceSha": "d230240859df4d2ab6aa4d78ed805aaeec90020a493bd8498a44ffc5337f8a4e",
      "output": {
        "bytes": 22751,
        "path": "data/global/tiles/Act4/Diab/RogC-chaosw.ds1",
        "sha256": "cc8cab1d07c9a57653677165837ad95c1465bcf422e5ad12cbfbf4c36e3cdcaa"
      },
      "sourceBytes": 23211,
      "operation": "clear",
      "walls": []
    },
    "data/global/tiles/Act4/Diab/RogHeart.ds1": {
      "archive": "d2exp.mpq",
      "member": "data/global/tiles/act4/diab/heart.ds1",
      "sourceSha": "cdd0b9877a84e08f40595972ca8ddfd0df410eb92a59bab9463197392e6bed1a",
      "output": {
        "bytes": 12751,
        "path": "data/global/tiles/Act4/Diab/RogHeart.ds1",
        "sha256": "2edab78af23f11f77310adbec14fdd21c70f4816669547dac85b9bbea08fa4ad"
      },
      "sourceBytes": 13211,
      "operation": "clear",
      "walls": []
    },
    "data/global/tiles/Act4/Lava/RogC-forgee.ds1": {
      "archive": "d2data.mpq",
      "member": "data/global/tiles/act4/lava/forgee.ds1",
      "sourceSha": "6c1e6f787b554cb018e68aeb620d0cddd9c0555ccdf184faa49ed0c65dc2a528",
      "output": {
        "bytes": 12753,
        "path": "data/global/tiles/Act4/Lava/RogC-forgee.ds1",
        "sha256": "03beccaed3898893dbbec13a7f3c02c409b13f5ce9331a6164c95ff4ba45bbe1"
      },
      "sourceBytes": 13233,
      "operation": "clear",
      "walls": []
    },
    "data/global/tiles/Act4/Lava/RogC-forgew.ds1": {
      "archive": "d2data.mpq",
      "member": "data/global/tiles/act4/lava/forgew.ds1",
      "sourceSha": "4499f1e4be9266ef6582bb51959254c929ea33334d85d2541ccfd344a1ae14c2",
      "output": {
        "bytes": 12798,
        "path": "data/global/tiles/Act4/Lava/RogC-forgew.ds1",
        "sha256": "7467b071d139843dfa20fdff9313a1d52f0109060e2cfc5fa2109f42322a4e0e"
      },
      "sourceBytes": 13098,
      "operation": "clear",
      "walls": []
    },
    "data/global/tiles/Expansion/icecave/RogC-icepool.ds1": {
      "archive": "d2exp.mpq",
      "member": "data/global/tiles/expansion/icecave/poolroom01.ds1",
      "sourceSha": "46099fc4b026ba3b73c0faf4e5357857b6e66caa316deee9c6b0e34b5da7bb36",
      "output": {
        "bytes": 17517,
        "path": "data/global/tiles/Expansion/icecave/RogC-icepool.ds1",
        "sha256": "c339b446acab8258406a30484389a6d2c45a8bed9e835ebb6cf671d335e6f9a3"
      },
      "sourceBytes": 19077,
      "operation": "clear",
      "walls": [
        [
          0,
          25,
          0
        ],
        [
          0,
          26,
          0
        ]
      ]
    },
    "data/global/tiles/Expansion/icecave/RogC-icetheme.ds1": {
      "archive": "d2exp.mpq",
      "member": "data/global/tiles/expansion/icecave/icentheme.ds1",
      "sourceSha": "a9e26e0a4031b688b5a99b66286f8e98c15f907234aab4320361bfe3b0f4b7ed",
      "output": {
        "bytes": 4717,
        "path": "data/global/tiles/Expansion/icecave/RogC-icetheme.ds1",
        "sha256": "316d69a84f823aa4733009f7492574b1e9c8fcf0f04f7c04302a3d2a667d420d"
      },
      "sourceBytes": 4997,
      "operation": "clear",
      "walls": []
    },
    "data/global/tiles/Expansion/Map/RogAncients.ds1": {
      "archive": "pd2maps.mpq",
      "member": "data/global/tiles/expansion/map/uberancients.ds1",
      "sourceSha": "90ad0884e3c138cd6a123b28acd9688bb009106e407bab68a145bd38c046f17b",
      "output": {
        "bytes": 15128,
        "path": "data/global/tiles/Expansion/Map/RogAncients.ds1",
        "sha256": "18af354c756e4a8cbf7a003e3cb5e020ea90974f4f3c2e27eaeaeb7e470f4e12"
      },
      "sourceBytes": 15148,
      "operation": "clear",
      "walls": []
    },
    "data/global/tiles/Expansion/Map/RogC-djinn.ds1": {
      "archive": "pd2maps.mpq",
      "member": "data/global/tiles/expansion/map/djinnsdomainboss.ds1",
      "sourceSha": "d277b9b28a2e7c556cdd7544ffa832757392fd9c077714f7bd9a17583cd14e02",
      "output": {
        "bytes": 18005,
        "path": "data/global/tiles/Expansion/Map/RogC-djinn.ds1",
        "sha256": "aa465dcff291a25a516d8b42fd1576cdc663a2492a51e6a98e0540a9e6ddf5d1"
      },
      "sourceBytes": 19925,
      "operation": "clear",
      "walls": [
        [
          0,
          2,
          8
        ]
      ]
    },
    "data/global/tiles/Expansion/Map/RogC-imperial.ds1": {
      "archive": "pd2maps.mpq",
      "member": "data/global/tiles/expansion/map/imperialpalacebossroom.ds1",
      "sourceSha": "d72dc30747e8835671facb1b7568c9e072cc0de14b1735ffeb2c1f913ac9a590",
      "output": {
        "bytes": 19340,
        "path": "data/global/tiles/Expansion/Map/RogC-imperial.ds1",
        "sha256": "914fdfb6bfe5070509571b728a04e63dd0d12bb835d16f46576f3c1867543361"
      },
      "sourceBytes": 20040,
      "operation": "clear",
      "walls": []
    },
    "data/global/tiles/Expansion/Map/RogC-monastery.ds1": {
      "archive": "pd2maps.mpq",
      "member": "data/global/tiles/expansion/map/monasterybase.ds1",
      "sourceSha": "54fb27f1f557f02b26b737e1445fd9b081b9087da7605c3ba1c588e7c0a911d5",
      "output": {
        "bytes": 12167,
        "path": "data/global/tiles/Expansion/Map/RogC-monastery.ds1",
        "sha256": "82396234ca6d537f48a7497cf1c964efd55a87d68b511a7b587fa7719f4acdd1"
      },
      "sourceBytes": 13207,
      "operation": "clear",
      "walls": [
        [
          0,
          17,
          13
        ],
        [
          0,
          18,
          13
        ]
      ]
    },
    "data/global/tiles/Expansion/mountaintop/RogC-arreattop.ds1": {
      "archive": "d2exp.mpq",
      "member": "data/global/tiles/expansion/mountaintop/mtntop.ds1",
      "sourceSha": "1c9588f0cc0abcc6dd2ee8070be36b6ed4dc987a9300ecc80b7abed15c08095f",
      "output": {
        "bytes": 22276,
        "path": "data/global/tiles/Expansion/mountaintop/RogC-arreattop.ds1",
        "sha256": "91eedffda304689985bdabf6b9dad9a0373b142330b32fd267db4ec4bc73acb8"
      },
      "sourceBytes": 22396,
      "operation": "clear",
      "walls": [
        [
          1,
          9,
          5
        ],
        [
          1,
          11,
          25
        ]
      ]
    },
    "data/global/tiles/Expansion/Siege/RogC-snow.ds1": {
      "archive": "d2exp.mpq",
      "member": "data/global/tiles/expansion/siege/siege_32x32eboss.ds1",
      "sourceSha": "417745d8a8a4f78136611fe2a19c2488927b0dd3967eaadb4037410ce35b2d9b",
      "output": {
        "bytes": 22011,
        "path": "data/global/tiles/Expansion/Siege/RogC-snow.ds1",
        "sha256": "f9a320c3a35c083a81de93c29dd21267a08853a88eb806a6b9f69377181804ad"
      },
      "sourceBytes": 22051,
      "operation": "clear",
      "walls": []
    },
    "data/global/tiles/Expansion/wildtemple/RogC-wildtemple.ds1": {
      "archive": "d2exp.mpq",
      "member": "data/global/tiles/expansion/wildtemple/tempenter.ds1",
      "sourceSha": "f698f1e97b8e5e6aaf6d1f0d88c25b9d2053913681e9999f7082e2284db81599",
      "output": {
        "bytes": 26250,
        "path": "data/global/tiles/Expansion/wildtemple/RogC-wildtemple.ds1",
        "sha256": "7c45767add680f559c362aa554df429a01bfb6ac95006f609cd14014cad21dd9"
      },
      "sourceBytes": 27250,
      "operation": "clear",
      "walls": [
        [
          0,
          11,
          6
        ],
        [
          0,
          12,
          6
        ]
      ]
    }
  }
}
''')
