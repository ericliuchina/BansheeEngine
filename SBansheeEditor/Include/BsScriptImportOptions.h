#pragma once

#include "BsScriptEditorPrerequisites.h"
#include "BsScriptObject.h"
#include "BsPixelData.h"
#include "BsGpuProgram.h"

namespace BansheeEngine
{
	class BS_SCR_BED_EXPORT ScriptImportOptionsBase : public ScriptObjectBase
	{
	public:
		SPtr<ImportOptions> getImportOptions() const { return mImportOptions; }

	protected:
		ScriptImportOptionsBase(MonoObject* instance);
		virtual ~ScriptImportOptionsBase() {}

		SPtr<ImportOptions> mImportOptions;
	};

	class BS_SCR_BED_EXPORT ScriptImportOptions : public ScriptObject <ScriptImportOptions, ScriptImportOptionsBase>
	{
	public:
		static String getAssemblyName() { return EDITOR_ASSEMBLY; }
		static String getNamespace() { return "BansheeEditor"; }
		static String getTypeName() { return "ImportOptions"; }
		static void initRuntimeData() { }

		static MonoObject* create(const SPtr<ImportOptions>& importOptions);

	private:
		ScriptImportOptions(MonoObject* instance);
	};

	class BS_SCR_BED_EXPORT ScriptTextureImportOptions : public ScriptObject<ScriptTextureImportOptions, ScriptImportOptionsBase>
	{
	public:
		SCRIPT_OBJ(EDITOR_ASSEMBLY, "BansheeEditor", "TextureImportOptions")

		static MonoObject* create();
		static MonoObject* create(const SPtr<TextureImportOptions>& options);

	private:
		SPtr<TextureImportOptions> getTexImportOptions();

		static void internal_CreateInstance(MonoObject* instance);
		static PixelFormat internal_GetPixelFormat(ScriptTextureImportOptions* thisPtr);
		static void internal_SetPixelFormat(ScriptTextureImportOptions* thisPtr, PixelFormat value);
		static bool internal_GetGenerateMipmaps(ScriptTextureImportOptions* thisPtr);
		static void internal_SetGenerateMipmaps(ScriptTextureImportOptions* thisPtr, bool value);
		static UINT32 internal_GetMaxMipmapLevel(ScriptTextureImportOptions* thisPtr);
		static void internal_SetMaxMipmapLevel(ScriptTextureImportOptions* thisPtr, UINT32 value);

		ScriptTextureImportOptions(MonoObject* instance);
	};

	class BS_SCR_BED_EXPORT ScriptFontImportOptions : public ScriptObject <ScriptFontImportOptions, ScriptImportOptionsBase>
	{
	public:
		SCRIPT_OBJ(EDITOR_ASSEMBLY, "BansheeEditor", "FontImportOptions")

		static MonoObject* create();
		static MonoObject* create(const SPtr<FontImportOptions>& options);

	private:
		SPtr<FontImportOptions> getFontImportOptions();

		static void internal_CreateInstance(MonoObject* instance);
		static MonoArray* internal_GetFontSizes(ScriptFontImportOptions* thisPtr);
		static void internal_SetFontSizes(ScriptFontImportOptions* thisPtr, MonoArray* value);
		static UINT32 internal_GetDPI(ScriptFontImportOptions* thisPtr);
		static void internal_SetDPI(ScriptFontImportOptions* thisPtr, UINT32 value);
		static bool internal_GetAntialiasing(ScriptFontImportOptions* thisPtr);
		static void internal_SetAntialiasing(ScriptFontImportOptions* thisPtr, bool value);
		static MonoArray* internal_GetCharRanges(ScriptFontImportOptions* thisPtr);
		static void internal_SetCharRanges(ScriptFontImportOptions* thisPtr, MonoArray* value);

		ScriptFontImportOptions(MonoObject* instance);
	};

	class BS_SCR_BED_EXPORT ScriptScriptCodeImportOptions : public ScriptObject <ScriptScriptCodeImportOptions, ScriptImportOptionsBase>
	{
	public:
		SCRIPT_OBJ(EDITOR_ASSEMBLY, "BansheeEditor", "ScriptCodeImportOptions")

		static MonoObject* create();
		static MonoObject* create(const SPtr<ScriptCodeImportOptions>& options);

	private:
		SPtr<ScriptCodeImportOptions> getCodeImportOptions();

		static void internal_CreateInstance(MonoObject* instance);
		static bool internal_IsEditorScript(ScriptScriptCodeImportOptions* thisPtr);
		static void internal_SetEditorScript(ScriptScriptCodeImportOptions* thisPtr, bool value);

		ScriptScriptCodeImportOptions(MonoObject* instance);
	};
}