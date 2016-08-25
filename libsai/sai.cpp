#include "sai.hpp"

#include <algorithm>

namespace sai
{
	// File Entry
	VirtualFileEntry::VirtualFileEntry()
		:
		Position(0)
	{
	}

	VirtualFileEntry::~VirtualFileEntry()
	{
	}

	uint32_t VirtualFileEntry::GetFlags() const
	{
		return Data.Flags;
	}

	const char * VirtualFileEntry::GetName() const
	{
		return Data.Name;
	}

	VirtualFileEntry::EntryType VirtualFileEntry::GetType() const
	{
		return Data.Type;
	}

	uint32_t VirtualFileEntry::GetCluster() const
	{
		return Data.Cluster;
	}

	uint32_t VirtualFileEntry::GetSize() const
	{
		return Data.Size;
	}

	time_t VirtualFileEntry::GetTimeStamp() const
	{
		return Data.TimeStamp / 10000000ULL - 11644473600ULL;
	}

	inline uint32_t VirtualFileEntry::Tell() const
	{
		return Position;
	}

	inline void VirtualFileEntry::Seek(uint32_t Offset)
	{
		Position = Offset;
	}

	// File System
	VirtualFileSystem::VirtualFileSystem()
		:
		CacheTable(nullptr),
		CacheBuffer(nullptr)
	{
		CacheTable = new FileSystemCluster();
		CacheBuffer = new FileSystemCluster();
	}

	VirtualFileSystem::~VirtualFileSystem()
	{
		if( FileStream )
		{
			FileStream.close();
		}
		if( CacheTable )
		{
			delete CacheTable;
		}
		if( CacheBuffer )
		{
			delete CacheBuffer;
		}
	}

	bool VirtualFileSystem::Mount(const char *FileName)
	{
		if( FileStream )
		{
			FileStream.close();
		}

		FileStream.open(FileName, std::ios::binary | std::ios::ate);

		if( FileStream )
		{
			std::ifstream::pos_type FileSize = FileStream.tellg();

			if( FileSize & 0x1FF )
			{
				// File size is not cluster-aligned
				FileStream.close();
				return false;
			}

			ClusterCount = static_cast<size_t>(FileSize) / FileSystemCluster::ClusterSize;

			// Verify all clusters
			for( size_t i = 0; i < ClusterCount; i++ )
			{
				GetCluster(i, CacheBuffer);
				if( i & 0x1FF ) // Cluster is data
				{
					if( CacheTable->TableEntries[i & 0x1FF].Checksum != CacheBuffer->Checksum(false) )
					{
						// Checksum mismatch. Data invalid
						FileStream.close();
						return false;
					}
				}
				else // Cluster is a table
				{
					if( CacheTable->TableEntries[0].Checksum != CacheTable->Checksum(true) )
					{
						// Checksum mismatch. Table invalid
						FileStream.close();
						return false;
					}
				}
			}
			return true;
		}
		return false;
	}

	size_t VirtualFileSystem::GetClusterCount() const
	{
		return ClusterCount;
	}

	size_t VirtualFileSystem::GetSize() const
	{
		return GetClusterCount() * FileSystemCluster::ClusterSize;
	}

	bool VirtualFileSystem::GetEntry(const char *Path, FileEntry &Entry)
	{
		if( FileStream )
		{
			GetCluster(2, CacheBuffer);

			std::string CurPath(Path);

			const char* CurToken = std::strtok(&CurPath[0], "./");

			size_t CurEntry = 0;
			while( CurEntry < 64 && CacheBuffer->FATEntries[CurEntry].Flags )
			{
				if( std::strcmp(CurToken, CacheBuffer->FATEntries[CurEntry].Name) == 0 )
				{
					if( (CurToken = std::strtok(nullptr, "./")) == nullptr ) // No more tokens to process, done
					{
						Entry.Data = CacheBuffer->FATEntries[CurEntry];
						return true;
					}

					if( CacheBuffer->FATEntries[CurEntry].Type != VirtualFileEntry::EntryType::Folder )
					{
						// Entry is not a folder, cant go further
						return false;
					}
					GetCluster(
						CacheBuffer->FATEntries[CurEntry].Cluster,
						CacheBuffer
					);
					CurEntry = 0;
					continue;
				}
				CurEntry++;
			}
		}
		return false;
	}

	bool VirtualFileSystem::Read(const FileEntry &Entry, size_t Offset, size_t Size, void *Destination)
	{
		if(
			FileStream
			&& Entry.GetCluster() < ClusterCount
			&& (Entry.GetType() == VirtualFileEntry::EntryType::File)
			&& ((Offset + Size) <= Entry.GetSize())
			)
		{
			uint8_t *WritePoint = reinterpret_cast<uint8_t*>(Destination);

			while( Size )
			{
				size_t CurCluster = Offset / FileSystemCluster::ClusterSize; // Nearest cluster Offset
				size_t CurClusterOffset = Offset % FileSystemCluster::ClusterSize; // Offset within cluster
				size_t CurClusterSize = std::min<size_t>(Size, FileSystemCluster::ClusterSize - CurClusterOffset); // Size within cluster

				// Current Cluster to read from
				GetCluster(Entry.GetCluster() + CurCluster, CacheBuffer);

				memcpy(WritePoint, CacheBuffer->u8 + CurClusterOffset, CurClusterSize);

				Size -= CurClusterSize;
				WritePoint += CurClusterSize;
				Offset += CurClusterSize;
				CurCluster++;
			}
			return true;
		}
		return false;
	}

	void VirtualFileSystem::Iterate(FileSystemVisitor &Visitor)
	{
		if( FileStream )
		{
			VisitCluster(2, Visitor);
		}
	}

	void VirtualFileSystem::VisitCluster(size_t ClusterNumber, FileSystemVisitor &Visitor)
	{
		FileSystemCluster CurCluster;
		GetCluster(ClusterNumber, &CurCluster);
		FileEntry CurEntry;
		for( size_t i = 0; CurCluster.FATEntries[i].Flags; i++ )
		{
			CurEntry.Data = CurCluster.FATEntries[i];
			switch( CurEntry.GetType() )
			{
			case FileEntry::EntryType::File:
			{
				Visitor.VisitFile(CurEntry);
				break;
			}
			case FileEntry::EntryType::Folder:
			{
				Visitor.VisitFolderBegin(CurEntry);
				VisitCluster(CurEntry.GetCluster(), Visitor);
				Visitor.VisitFolderEnd();
				break;
			}
			}
		}
	}

	bool VirtualFileSystem::GetCluster(size_t ClusterNum, FileSystemCluster *Cluster)
	{
		if( ClusterNum < ClusterCount )
		{
			if( ClusterNum & 0x1FF ) // Cluster is data
			{
				size_t NearestTable = ClusterNum & ~(0x1FF);
				uint32_t Key = 0;
				if( CacheTableNum == NearestTable ) // Table Cache Hit
				{
					Key = CacheTable->TableEntries[ClusterNum - NearestTable].Checksum;
				}
				else // Cache Miss
				{
					// Read and Decrypt Table
					FileStream.seekg(NearestTable * FileSystemCluster::ClusterSize);
					FileStream.read(
						reinterpret_cast<char*>(CacheTable->u8),
						FileSystemCluster::ClusterSize
					);
					CacheTable->DecryptTable(NearestTable);
					Key = CacheTable->TableEntries[ClusterNum - NearestTable].Checksum;
				}

				// Read and Decrypt Data
				FileStream.seekg(ClusterNum * FileSystemCluster::ClusterSize);
				FileStream.read(
					reinterpret_cast<char*>(Cluster->u8),
					FileSystemCluster::ClusterSize
				);
				Cluster->DecryptData(Key);
				return true;
			}
			else // Cluster is a table
			{
				if( ClusterNum == CacheTableNum ) // Cache hit
				{
					memcpy(Cluster->u8, CacheTable->u8, FileSystemCluster::ClusterSize);
					return true;
				}
				// Read and Decrypt Table
				FileStream.seekg(ClusterNum * FileSystemCluster::ClusterSize);
				FileStream.read(
					reinterpret_cast<char*>(CacheTable->u8),
					FileSystemCluster::ClusterSize
				);
				CacheTable->DecryptTable(ClusterNum);
				CacheTableNum = ClusterNum;
				memcpy(Cluster->u8, CacheTable->u8, FileSystemCluster::ClusterSize);
				return true;
			}
		}
		return false;
	}

	// Cluster

	void VirtualCluster::DecryptTable(uint32_t ClusterNumber)
	{
		ClusterNumber &= (~0x1FF);
		for( size_t i = 0; i < (ClusterSize / 4); i++ )
		{
			uint32_t CurCipher = u32[i];
			uint32_t X = ClusterNumber ^ CurCipher ^ (
				DecryptionKey[(ClusterNumber >> 24) & 0xFF]
				+ DecryptionKey[(ClusterNumber >> 16) & 0xFF]
				+ DecryptionKey[(ClusterNumber >> 8) & 0xFF]
				+ DecryptionKey[ClusterNumber & 0xFF]);

			u32[i] = static_cast<uint32_t>((X << 16) | (X >> 16));

			ClusterNumber = CurCipher;
		};
	}

	void VirtualCluster::DecryptData(uint32_t Key)
	{
		for( size_t i = 0; i < (ClusterSize / 4); i++ )
		{
			uint32_t CurCipher = u32[i];
			u32[i] =
				CurCipher
				- (Key ^ (
					DecryptionKey[Key & 0xFF]
					+ DecryptionKey[(Key >> 8) & 0xFF]
					+ DecryptionKey[(Key >> 16) & 0xFF]
					+ DecryptionKey[(Key >> 24) & 0xFF]));
			Key = CurCipher;
		}
	}

	uint32_t VirtualCluster::Checksum(bool Table)
	{
		uint32_t Accumulate = 0;
		for( size_t i = (Table ? 1 : 0); i < (ClusterSize / 4); i++ )
		{
			Accumulate = (2 * Accumulate | (Accumulate >> 31)) ^ u32[i];
		}
		return Accumulate | 1;
	}

	const uint32_t VirtualCluster::DecryptionKey[1024] =
	{
		0x9913D29E,0x83F58D3D,0xD0BE1526,0x86442EB7,0x7EC69BFB,0x89D75F64,0xFB51B239,0xFF097C56,
		0xA206EF1E,0x973D668D,0xC383770D,0x1CB4CCEB,0x36F7108B,0x40336BCD,0x84D123BD,0xAFEF5DF3,
		0x90326747,0xCBFFA8DD,0x25B94703,0xD7C5A4BA,0xE40A17A0,0xEADAE6F2,0x6B738250,0x76ECF24A,
		0x6F2746CC,0x9BF95E24,0x1ECA68C5,0xE71C5929,0x7817E56C,0x2F99C471,0x395A32B9,0x61438343,
		0x5E3E4F88,0x80A9332C,0x1879C69F,0x7A03D354,0x12E89720,0xF980448E,0x03643576,0x963C1D7B,
		0xBBED01D6,0xC512A6B1,0x51CB492B,0x44BADEC9,0xB2D54BC1,0x4E7C2893,0x1531C9A3,0x43A32CA5,
		0x55B25A87,0x70D9FA79,0xEF5B4AE3,0x8AE7F495,0x923A8505,0x1D92650C,0xC94A9A5C,0x27D4BB14,
		0x1372A9F7,0x0C19A7FE,0x64FA1A53,0xF1A2EB6D,0x9FEB910F,0x4CE10C4E,0x20825601,0x7DFC98C4,
		0xA046C808,0x8E90E7BE,0x601DE357,0xF360F37C,0x00CD6F77,0xCC6AB9D4,0x24CC4E78,0xAB1E0BFC,
		0x6A8BC585,0xFD70ABF0,0xD4A75261,0x1ABF5834,0x45DCFE17,0x5F67E136,0x948FD915,0x65AD9EF5,
		0x81AB20E9,0xD36EAF42,0x0F7F45C7,0x1BAE72D9,0xBE116AC6,0xDF58B4D5,0x3F0B960E,0xC2613F98,
		0xB065F8B0,0x6259F975,0xC49AEE84,0x29718963,0x0B6D991D,0x09CF7A37,0x692A6DF8,0x67B68B02,
		0x2E10DBC2,0x6C34E93C,0xA84B50A1,0xAC6FC0BB,0x5CA6184C,0x34E46183,0x42B379A9,0x79883AB6,
		0x08750921,0x35AF2B19,0xF7AA886A,0x49F281D3,0xA1768059,0x14568CFD,0x8B3625F6,0x3E1B2D9D,
		0xF60E14CE,0x1157270A,0xDB5C7EB3,0x738A0AFA,0x19C248E5,0x590CBD62,0x7B37C312,0xFC00B148,
		0xD808CF07,0xD6BD1C82,0xBD50F1D8,0x91DEA3B8,0xFA86B340,0xF5DF2A80,0x9A7BEA6E,0x1720B8F1,
		0xED94A56B,0xBF02BE28,0x0D419FA8,0x073B4DBC,0x829E3144,0x029F43E1,0x71E6D51F,0xA9381F09,
		0x583075E0,0xE398D789,0xF0E31106,0x75073EB5,0x5704863E,0x6EF1043B,0xBC407F33,0x8DBCFB25,
		0x886C8F22,0x5AF4DD7A,0x2CEACA35,0x8FC969DC,0x9DB8D6B4,0xC65EDC2F,0xE60F9316,0x0A84519A,
		0x3A294011,0xDCF3063F,0x41621623,0x228CB75B,0x28E9D166,0xAE631B7F,0x06D8C267,0xDA693C94,
		0x54A5E860,0x7C2170F4,0xF2E294CB,0x5B77A0F9,0xB91522A6,0xEC549500,0x10DD78A7,0x3823E458,
		0x77D3635A,0x018E3069,0xE039D055,0xD5C341BF,0x9C2400EA,0x85C0A1D1,0x66059C86,0x0416FF1A,
		0xE27E05C8,0xB19C4C2D,0xFE4DF58F,0xD2F0CE2A,0x32E013C0,0xEED637D7,0xE9FEC1E8,0xA4890DCA,
		0xF4180313,0x7291738C,0xE1B053A2,0x9801267E,0x2DA15BDB,0xADC4DA4F,0xCF95D474,0xC0265781,
		0x1F226CED,0xA7472952,0x3C5F0273,0xC152BA68,0xDD66F09B,0x93C7EDCF,0x4F147404,0x3193425D,
		0x26B5768A,0x0E683B2E,0x952FDF30,0x2A6BAE46,0xA3559270,0xB781D897,0xEB4ECB51,0xDE49394D,
		0x483F629C,0x2153845E,0xB40D64E2,0x47DB0ED0,0x302D8E4B,0x4BF8125F,0x2BD2B0AC,0x3DC836EC,
		0xC7871965,0xB64C5CDE,0x9EA8BC27,0xD1853490,0x3B42EC6F,0x63A4FD91,0xAA289D18,0x4D2B1E49,
		0xB8A060AD,0xB5F6C799,0x6D1F7D1C,0xBA8DAAE6,0xE51A0FC3,0xD94890E7,0x167DF6D2,0x879BCD41,
		0x5096AC1B,0x05ACB5DA,0x375D24EE,0x7F2EB6AA,0xA535F738,0xCAD0AD10,0xF8456E3A,0x23FD5492,
		0xB3745532,0x53C1A272,0x469DFCDF,0xE897BF7D,0xA6BBE2AE,0x68CE38AF,0x5D783D0B,0x524F21E4,
		0x4A257B31,0xCE7A07B2,0x562CE045,0x33B708A4,0x8CEE8AEF,0xC8FB71FF,0x74E52FAB,0xCDB18796,
		0xD1FD71B9,0xA16841D9,0xAA60C5E0,0x2BD8C98E,0x7B3DF482,0xC238D7FA,0x7CB07208,0xB9C9EACE,
		0xBA89B0C3,0x2514E692,0x79FA9D58,0x35B577F2,0x3B7DC791,0x98BD0EC1,0x92B38905,0xC842B626,
		0x1A36BB98,0x28FE1E5B,0x5D0E2BA8,0x76E1D3A1,0x30DB461A,0x7595F81B,0x38EEB344,0x1C56950A,
		0x9C2B6E22,0xFBC109FD,0xAED01C77,0x9A7CA842,0x4E748FBB,0xFD318EA2,0xC90B5B1F,0xE76FDC2E,
		0x74847696,0x0C345899,0x8CD37802,0x632DB41D,0x435A1829,0x510A2C31,0x679CE8F8,0xA21EF07E,
		0x5EF0635A,0xFE8DA5F5,0x2F05BF66,0x8A336849,0xBC271A8B,0xEF108B46,0x05934B9C,0xA871AE3C,
		0xB60CDFAA,0x114414BA,0x95DA54A0,0xD05B8655,0x50CE8D6F,0x73FF992A,0xBBC45F79,0x3EBF56AD,
		0xC499B87B,0xC3DD4514,0x34F93E8A,0x87E86C48,0x2E1BA161,0x62F83147,0x914B3AEB,0x5A8FE2BC,
		0xA92FDDF7,0xB8648CD5,0x7E65C4B6,0xDBC71941,0x41D12AB3,0xCF79476B,0xF3D9F576,0x776A05CA,
		0xE91670DB,0xF94D215D,0xB298A900,0x6628B1D2,0xD978A423,0x59BC5A19,0x9B2EADB5,0x860D15C0,
		0x04839451,0x65302E37,0x4F1AB752,0x6EF383E3,0xAF26C1AE,0x31EC1635,0xD6EDA06D,0x6B590C4C,
		0x99E31790,0xE6F452C9,0x08BBD41C,0xCD29ECFC,0x0FCF00A4,0xB1901F9D,0xC537C8AF,0x40ABFD70,
		0x887E26DE,0xDCF51BD3,0x2D3961C7,0xA3BA43CD,0x89C25D0B,0xF61C239E,0xF2519374,0x52ACBD83,
		0x946DC011,0x33924EDD,0x2CE936E9,0x42083909,0x3D11A7E1,0xCBF6D8B1,0x54AFBAC2,0xA53592C6,
		0xD2A5E06C,0xB3C56B81,0xCCE6CE39,0xDDE07563,0x3FCA98D0,0x475E35CF,0x0D8E97C5,0x375033C8,
		0xC109C3A6,0xDE002575,0x53DF2257,0x5CA94FE6,0x4C7A6917,0x6C430FB0,0xB5734293,0xEA23F71E,
		0xFFA7047A,0x7A54AA56,0xBDC84920,0x6A1FD9FB,0x16CC0340,0x908A068D,0x4A7029AC,0x03137F87,
		0x02BE4C04,0x39C69EF0,0x569767E4,0x4D763FAB,0x24D67CD6,0x9D06CF32,0xBE4AC6EA,0x78E75071,
		0xAD9F5572,0x23078295,0xCAD49A84,0x20B8D280,0x1945EF34,0x823AFB54,0x859E3407,0x0B178836,
		0x0AB738F4,0xE032E3A7,0xF4A853CC,0x27621D97,0x22673B53,0xB7A68143,0xF572F930,0x6418B96A,
		0x069D4D65,0xAB20FAB4,0xED91EE85,0x9712D178,0x176B2013,0x4955F2D4,0xDA04967C,0x81EB1062,
		0x1F6ECA03,0x01F27A67,0x93B162FE,0x3ACB3289,0xE5B28A24,0xF7A460D1,0x8D630D18,0x451DE433,
		0x00A1598F,0x3696BCF3,0x296CA2E8,0xBFCDF6EC,0xC6FB01A5,0xAC027E3D,0x7FD70B6E,0xA4E4A610,
		0x4B0F874B,0x074657EF,0xB040ABD8,0xE8B4EB4F,0xC04E852D,0xE39ABE3B,0x0E49372C,0x13246A01,
		0xF83E4064,0xF0B65CF6,0x8BE50259,0x68C0E5B2,0x8E257D60,0xA682B50D,0x71E2D594,0x1E809C0C,
		0x9E3CFE86,0xC75FDB15,0x2A58274A,0x21D52D3A,0x3219F368,0x837F6F27,0x58EAB23F,0x698651DC,
		0x558C743E,0x1869E945,0x1B9BD088,0x9F22DE0F,0xD8EFCD50,0x1075AC9B,0x2603648C,0x80772FBF,
		0xD481FF21,0xEBAE9B5C,0x6F48E15F,0x60DC124D,0x5FAD0825,0xFCDE9F12,0xE1C391F1,0xE4614438,
		0x5785907D,0x15F1DAEE,0x845311E7,0xB4943D73,0xD387CB2B,0xF13BFC0E,0x44D2CCDA,0x093FE72F,
		0xEEA073B8,0xCE2184E5,0x48AA0AED,0xE288139F,0xD72A487F,0x1D7BAFE2,0x8F4FA3CB,0x7D8B6D28,
		0x61B92869,0x72A266DF,0x14FC30FF,0xA04C65D7,0x12F75EF9,0x705724BD,0xA7157B06,0xFA5C075E,
		0xDF66EDBE,0x5BA3C24E,0x6D524AC4,0xD541F19A,0xEC5D3CB7,0x4601D616,0x3C2C79A9,0x964780A3,
		0x021CF107,0xE9253648,0x8AFBA619,0x8CF31842,0xBF40F860,0xA672F03E,0xFA2756AC,0x927B2E7E,
		0x1E37D3C4,0x7C3A0524,0x4F284D1B,0xD8A31E9D,0xBA73B6E6,0xF399710D,0xBD8B1937,0x70FFE130,
		0x056DAA4A,0xDC509CA1,0x07358DFF,0xDF30A2DC,0x67E7349F,0x49532C31,0x2393EBAA,0xE54DF202,
		0x3A2C7EC9,0x98AB13EF,0x7FA52975,0x83E4792E,0x7485DA08,0x4A1823A8,0x77812011,0x8710BB89,
		0x9B4E0C68,0x64125D8E,0x5F174A0E,0x33EA50E7,0xA5E168B0,0x1BD9B944,0x6D7D8FE0,0xEE66B84C,
		0xF0DB530C,0xF8B06B72,0x97ED7DF8,0x126E0122,0x364BED23,0xA103B75C,0x3BC844FA,0xD0946501,
		0x4E2F70F1,0x79A6F413,0x60B9E977,0xC1582F10,0x759B286A,0xE723EEF5,0x8BAC4B39,0xB074B188,
		0xCC528E64,0x698700EE,0x44F9E5BB,0x7E336153,0xE2413AFD,0x91DCE2BE,0xFDCE9EC1,0xCAB2DE4F,
		0x46C5A486,0xA0D630DB,0x1FCD5FCA,0xEA110891,0x3F20C6F9,0xE8F1B25D,0x6EFD10C8,0x889027AF,
		0xF284AF3F,0x89EE9A61,0x58AF1421,0xE41B9269,0x260C6D71,0x5079D96E,0xD959E465,0x519CD72C,
		0x73B64F5A,0x40BE5535,0x78386CBC,0x0A1A02CF,0xDBC126B6,0xAD02BC8D,0x22A85BC5,0xA28ABEC3,
		0x5C643952,0xE35BC9AD,0xCBDACA63,0x4CA076A4,0x4B6121CB,0x9500BF7D,0x6F8E32BF,0xC06587E5,
		0x21FAEF46,0x9C2AD2F6,0x7691D4A2,0xB13E4687,0xC7460AD6,0xDDFE54D5,0x81F516F3,0xC60D7438,
		0xB9CB3BC7,0xC4770D94,0xF4571240,0x06862A50,0x30D343D3,0x5ACF52B2,0xACF4E68A,0x0FC2A59B,
		0xB70AEACD,0x53AA5E80,0xCF624E8F,0xF1214CEB,0x936072DF,0x62193F18,0xF5491CDA,0x5D476958,
		0xDA7A852D,0x5B053E12,0xC5A9F6D0,0xABD4A7D1,0xD25E6E82,0xA4D17314,0x2E148C4E,0x6B9F6399,
		0xBC26DB47,0x8296DDCE,0x3E71D616,0x350E4083,0x2063F503,0x167833F2,0x115CDC5E,0x4208E715,
		0x03A49B66,0x43A724BA,0xA3B71B8C,0x107584AE,0xC24AE0C6,0xB3FC6273,0x280F3795,0x1392C5D4,
		0xD5BAC762,0xB46B5A3B,0xC9480B8B,0xC39783FC,0x17F2935B,0x9DB482F4,0xA7E9CC09,0x553F4734,
		0x8DB5C3A3,0x7195EC7A,0xA8518A9A,0x0CE6CB2A,0x14D50976,0x99C077A5,0x012E1733,0x94EC3D7C,
		0x3D825805,0x0E80A920,0x1D39D1AB,0xFCD85126,0x3C7F3C79,0x7A43780B,0xB26815D9,0xAF1F7F1C,
		0xBB8D7C81,0xAAE5250F,0x34BC670A,0x1929C8D2,0xD6AE9FC0,0x1AE07506,0x416F3155,0x9EB38698,
		0x8F22CF29,0x04E8065F,0xE07CFBDE,0x2AEF90E8,0x6CAD049C,0x4DC3A8CC,0x597E3596,0x08562B92,
		0x52A21D6F,0xB6C9881D,0xFBD75784,0xF613FC32,0x54C6F757,0x66E2D57B,0xCD69FE9E,0x478CA13D,
		0x2F5F6428,0x8E55913C,0xF9091185,0x0089E8B3,0x1C6A48BD,0x3844946D,0x24CC8B6B,0x6524AC2B,
		0xD1F6A0F0,0x32980E51,0x8634CE17,0xED67417F,0x250BAEB9,0x84D2FD1A,0xEC6C4593,0x29D0C0B1,
		0xEBDF42A9,0x0D3DCD45,0x72BF963A,0x27F0B590,0x159D5978,0x3104ABD7,0x903B1F27,0x9F886A56,
		0x80540FA6,0x18F8AD1F,0xEF5A9870,0x85016FC2,0xC8362D41,0x6376C497,0xE1A15C67,0x6ABD806C,
		0x569AC1E2,0xFE5D1AF7,0x61CADF59,0xCE063874,0xD4F722DD,0x37DEC2EC,0xAE70BDEA,0x0B2D99B4,
		0x39B895FE,0x091E9DFB,0xA9150754,0x7D1D7A36,0x9A07B41E,0x5E8FE3B5,0xD34503A0,0xBE2BFAB7,
		0x5742D0A7,0x48DDBA25,0x7BE3604D,0x2D4C66E9,0xB831FFB8,0xF7BBA343,0x451697E4,0x2C4FD84B,
		0x96B17B00,0xB5C789E3,0xFFEBF9ED,0xD7C4B349,0xDE3281D8,0x689E4904,0xE683F32F,0x2B3CB0E1,
		0x2F8E651D,0x7211B4E4,0xF635A101,0xDBB81A38,0x8E4981F0,0x11BE1949,0xF672E5FC,0xD9F082FA,
		0x11DC51AC,0xF92BC969,0x24081B1B,0xE3963FE6,0x7F7D39A5,0x5E9FFFCB,0x38100F7A,0xEB2E02CC,
		0xEC89C7DA,0x9AC5811B,0x707484A3,0x18B0460A,0x5C617FCC,0x93FCF2CC,0x0C100C16,0x02581227,
		0x1B7FF709,0xB244CD40,0x57D96DF6,0xF1C80701,0xC1CBF60E,0x05FB770C,0x49F5C965,0x824F54B3,
		0x5CBCB083,0x1EAE61D1,0x7C68643A,0x22B36D41,0x18BB8748,0xB0CA2BD7,0xC6C2CF89,0xCFBB2E5C,
		0x863A2245,0xCBA7869C,0x0D8405A9,0x5C0DFAC0,0x00BAE703,0xEC963296,0xADBDA550,0x94A2FFEF,
		0x0EF91FEC,0x213DA239,0x8561D716,0xCB6596DD,0x1CFEB477,0x752F6328,0xDB1D2C74,0x05BD83C0,
		0xD1159AD8,0xAEAA1BAF,0x174CBEB0,0x4028FDC1,0xB7B6D68C,0x66A7868A,0xEAFF6A57,0xBF2EDFA7,
		0x00000000,0x00000000,0x00401004,0x3F500401,0xFFFFFFFF,0x0048121A,0x0048122E,0xFFFFFFFF,
		0x0048126B,0x0048127F,0xFFFFFFFF,0x004813DD,0x004813F1,0x00000000,0xFFFFFFFF,0x00482384,
		0x00482398,0xFFFFFFFF,0x00482400,0x00482414,0xFFFFFFFF,0x004852DE,0x004852F2,0x00000000,
		0x0048464A,0x0048465E,0x00000000,0x00484731,0x00484745,0xFFFFFFFF,0x00484134,0x0048414A,
		0xFFFFFFFF,0x004841C2,0x004841D8,0xFFFFFFFF,0x00485702,0x00485718,0xFFFFFFFF,0x004857BA,
		0x004857D0,0xFFFFFFFF,0x00485A3B,0x00485A51,0xFFFFFFFF,0x00485AE1,0x00485AF7,0xFFFFFFFF,
		0x00485C04,0x00485C18,0xFFFFFFFF,0x00485C6C,0x00485C80,0xFFFFFFFF,0x00485D3D,0x00485D53,
		0xFFFFFFFF,0x00485E2F,0x00485E45,0x00000000,0xFFFFFFFF,0x00487CE2,0x00487CF6,0x00000000,
		0x00000001,0x00000001,0x00000003,0x00000004,0x00000001,0x00000002,0x00000001,0x00000003,
		0x00000001,0x00000004,0x00000001,0x00000006,0x00000001,0x00000008,0x00000001,0x0000000C,
		0x00000001,0x00000010,0x00000001,0x00000018,0x00000001,0x00000020,0x00000000,0x00000000,
		0xFFFFFFFF,0x004880E0,0x004880F4,0x00000000,0xFFFFFFFF,0x0048846A,0x0048847E,0x00000000,
		0xFFFFFFFF,0x00488565,0x00488579,0x00000000,0xFFFFFFFF,0x00488607,0x0048861B,0x00000000,
		0xFFFFFFFF,0x0048916F,0x00489183,0xFFFFFFFF,0x004894E7,0x004894FB,0xFFFFFFFF,0x0048C4E9,
		0x0048C4FD,0xFFFFFFFF,0x0048C5A4,0x0048C5B8,0xFFFFFFFF,0x00489F43,0x00489F57,0xFFFFFFFF,
		0x0048B5D5,0x0048B5E9,0xFFFFFFFF,0x0048A846,0x0048A85A,0xFFFFFFFF,0x0048AF0A,0x0048AF1E,
		0xFFFFFFFF,0x0048B810,0x0048B824,0xFFFFFFFF,0x0048D753,0x0048D767,0xFFFFFFFF,0x0048CBBF,
		0x0048CBD3,0xFFFFFFFF,0x0048C8E6,0x0048C8FA,0xFFFFFFFF,0x0048D056,0x0048D06A,0xFFFFFFFF,
		0x0048D0EC,0x0048D100,0xFFFFFFFF,0x0048CCCF,0x0048CCE3,0xFFFFFFFF,0x0048C820,0x0048C834,
		0xFFFFFFFF,0x0048D858,0x0048D86C,0x00000000,0xFFFFFFFF,0x0048DF89,0x0048DF9D,0x00000000,
		0x00000000,0x40300000,0x00000000,0x3FA00000,0xFFFFFFFF,0x0048E369,0x0048E37D,0x00000000,
		0xFFFFFFFF,0x0048E4AF,0x0048E4C2,0x00000000,0xFFFFFFFF,0x0048E73E,0x0048E752,0x00000000,
		0xFFFFFFFF,0x0048EBCB,0x0048EBDF,0x00000000,0x00000000,0xBFF00000,0x00000000,0x40990000,
		0xFFFFFFFF,0x0048F03B,0x0048F04F,0x00000000,0xFFFFFFFF,0x0048F0F7,0x0048F10B,0x00000000,
	};
}