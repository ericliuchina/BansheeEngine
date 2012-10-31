#pragma once

#include "CmPrerequisites.h"
#include "CmModule.h"

namespace CamelotEngine
{
	class Resources : public Module<Resources>
	{
	public:
		/**
		 * @brief	Constructor.
		 *
		 * @param	resMetaPath		Folder where the resource meta-data will be stored. If the folder doesn't exist
		 * 							it will be created.
		 */
		Resources(const String& metaDataFolder);
		~Resources();

		/**
		 * @brief	Loads the resource from a given path. Returns null if resource can't be loaded.
		 *
		 * @param	filePath					The path of the file to load. The file is searched for in
		 * 										the AssetDatabase first, and if it cannot be found it is
		 * 										loaded as a temporary resource object. You can't save
		 * 										references to temporary resource objects because they won't
		 * 										persist after application shut-down, but otherwise they act
		 * 										the same as normal resources.
		 *
		 * @return	Loaded resource, or null if it cannot be found.
		 */
		BaseResourceRef load(const String& filePath);

		/**
		 * @brief	Loads the resource with the given uuid.
		 *
		 * @param	uuid	UUID of the resource to load. 
		 *
		 * @return	Loaded resource, or null if it cannot be found.
		 */
		BaseResourceRef loadFromUUID(const String& uuid);

		/**
		 * @brief	Saves the resource at the specified location.
		 *
		 * @param	resource	The resource.
		 * @param	filePath	Full pathname of the file.
		 */
		void save(BaseResourceRef resource, const String& filePath);

	public:
		struct ResourceMetaData : public IReflectable
		{
			String mUUID;
			String mPath;

			/************************************************************************/
			/* 								SERIALIZATION                      		*/
			/************************************************************************/
		public:
			friend class ResourceMetaDataRTTI;
			static RTTITypeBase* getRTTIStatic();
			virtual RTTITypeBase* getRTTI() const;
		};

	private:
		typedef std::shared_ptr<ResourceMetaData> ResourceMetaDataPtr;
		map<String, ResourceMetaDataPtr>::type mResourceMetaData;

		void loadMetaData();
		void saveMetaData(const ResourceMetaDataPtr metaData);

		void addMetaData(const String& uuid, const String& filePath);
		void updateMetaData(const String& uuid, const String& newFilePath);

		bool exists(const String& uuid) const;

		String mMetaDataFolderPath;
	};

	CM_EXPORT Resources& gResources();
}