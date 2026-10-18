#include "ArenaFSR3Upscaler.h"
#include "ArenaUpscalingLog.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "SceneView.h"
#include "ShaderCompilerCore.h"
#include "ShaderParameterStruct.h"
#include "ScreenPass.h"

namespace ArenaFSR3
{
	enum EPass : int32
	{
		PassPrepareInputs = 0,
		PassLumaPyramid = 1,
		PassShadingChangePyramid = 2,
		PassShadingChange = 3,
		PassPrepareReactivity = 4,
		PassLumaInstability = 5,
		PassAccumulate = 6,
		PassRcas = 7,
	};

	static bool ShouldCompile(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5) && !IsMobilePlatform(Parameters.Platform);
	}

	static void SetPassDefines(int32 Pass, FShaderCompilerEnvironment& OutEnvironment)
	{
		OutEnvironment.SetDefine(TEXT("ARENA_FSR3_PASS"), Pass);
		OutEnvironment.CompilerFlags.Add(CFLAG_ForceDXC);
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// Shader parameters. Names match the globals in FidelityFX/fsr3upscaler/ffx_fsr3upscaler_callbacks_hlsl.ush.
// ---------------------------------------------------------------------------------------------------------------------

BEGIN_SHADER_PARAMETER_STRUCT(FArenaFSR3Constants, )
	SHADER_PARAMETER(FIntPoint, iRenderSize)
	SHADER_PARAMETER(FIntPoint, iPreviousFrameRenderSize)
	SHADER_PARAMETER(FIntPoint, iUpscaleSize)
	SHADER_PARAMETER(FIntPoint, iPreviousFrameUpscaleSize)
	SHADER_PARAMETER(FIntPoint, iMaxRenderSize)
	SHADER_PARAMETER(FIntPoint, iMaxUpscaleSize)
	SHADER_PARAMETER(FVector4f, fDeviceToViewDepth)
	SHADER_PARAMETER(FVector2f, fJitter)
	SHADER_PARAMETER(FVector2f, fPreviousFrameJitter)
	SHADER_PARAMETER(FVector2f, fMotionVectorScale)
	SHADER_PARAMETER(FVector2f, fDownscaleFactor)
	SHADER_PARAMETER(FVector2f, fMotionVectorJitterCancellation)
	SHADER_PARAMETER(float, fTanHalfFOV)
	SHADER_PARAMETER(float, fJitterSequenceLength)
	SHADER_PARAMETER(float, fDeltaTime)
	SHADER_PARAMETER(float, fDeltaPreExposure)
	SHADER_PARAMETER(float, fViewSpaceToMetersFactor)
	SHADER_PARAMETER(float, fFrameIndex)
	SHADER_PARAMETER(float, fVelocityFactor)
	SHADER_PARAMETER(float, fReactivenessScale)
	SHADER_PARAMETER(float, fShadingChangeScale)
	SHADER_PARAMETER(float, fAccumulationAddedPerFrame)
	SHADER_PARAMETER(float, fMinDisocclusionAccumulation)
	SHADER_PARAMETER_SAMPLER(SamplerState, s_PointClamp)
	SHADER_PARAMETER_SAMPLER(SamplerState, s_LinearClamp)
END_SHADER_PARAMETER_STRUCT()

BEGIN_SHADER_PARAMETER_STRUCT(FArenaFSR3SpdConstants, )
	SHADER_PARAMETER(uint32, mips)
	SHADER_PARAMETER(uint32, numWorkGroups)
	SHADER_PARAMETER(FUintVector2, workGroupOffset)
	SHADER_PARAMETER(FUintVector2, renderSize)
END_SHADER_PARAMETER_STRUCT()

// Converts Unreal's velocity/depth/exposure to FSR inputs (ArenaFSR3Inputs.usf).
class FArenaFSR3InputsCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FArenaFSR3InputsCS);
	SHADER_USE_PARAMETER_STRUCT(FArenaFSR3InputsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, VelocityTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, DepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, EyeAdaptationTexture)
		SHADER_PARAMETER(FIntPoint, InputViewMin)
		SHADER_PARAMETER(FIntPoint, InputViewSize)
		SHADER_PARAMETER(float, OneOverPreExposure)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, OutMotionVectors)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, OutDepth)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, OutExposure)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return ArenaFSR3::ShouldCompile(Parameters); }
};
IMPLEMENT_GLOBAL_SHADER(FArenaFSR3InputsCS, "/Plugin/ArenaUpscaling/Private/ArenaFSR3Inputs.usf", "MainCS", SF_Compute);

#define ARENA_FSR3_SHADER_BODY(ClassName, PassIndex) \
	public: \
	DECLARE_GLOBAL_SHADER(ClassName); \
	SHADER_USE_PARAMETER_STRUCT(ClassName, FGlobalShader); \
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return ArenaFSR3::ShouldCompile(Parameters); } \
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment) \
	{ \
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment); \
		ArenaFSR3::SetPassDefines(PassIndex, OutEnvironment); \
	}

class FArenaFSR3PrepareInputsCS : public FGlobalShader
{
	ARENA_FSR3_SHADER_BODY(FArenaFSR3PrepareInputsCS, ArenaFSR3::PassPrepareInputs)
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FArenaFSR3Constants, Constants)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, r_input_motion_vectors)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, r_input_depth)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, r_input_color_jittered)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, rw_dilated_motion_vectors)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, rw_dilated_depth)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, rw_reconstructed_previous_nearest_depth)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, rw_farthest_depth)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, rw_current_luma)
	END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FArenaFSR3PrepareInputsCS, "/Plugin/ArenaUpscaling/Private/ArenaFSR3Upscaler.usf", "MainCS", SF_Compute);

class FArenaFSR3LumaPyramidCS : public FGlobalShader
{
	ARENA_FSR3_SHADER_BODY(FArenaFSR3LumaPyramidCS, ArenaFSR3::PassLumaPyramid)
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FArenaFSR3Constants, Constants)
		SHADER_PARAMETER_STRUCT_INCLUDE(FArenaFSR3SpdConstants, Spd)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, r_current_luma)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, r_farthest_depth)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, rw_spd_global_atomic)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, rw_frame_info)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, rw_spd_mip0)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, rw_spd_mip1)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, rw_spd_mip2)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, rw_spd_mip3)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, rw_spd_mip4)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, rw_spd_mip5)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, rw_farthest_depth_mip1)
	END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FArenaFSR3LumaPyramidCS, "/Plugin/ArenaUpscaling/Private/ArenaFSR3Upscaler.usf", "MainCS", SF_Compute);

class FArenaFSR3ShadingChangePyramidCS : public FGlobalShader
{
	ARENA_FSR3_SHADER_BODY(FArenaFSR3ShadingChangePyramidCS, ArenaFSR3::PassShadingChangePyramid)
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FArenaFSR3Constants, Constants)
		SHADER_PARAMETER_STRUCT_INCLUDE(FArenaFSR3SpdConstants, Spd)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, r_current_luma)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, r_previous_luma)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, r_dilated_motion_vectors)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, r_input_exposure)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, rw_spd_global_atomic)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, rw_spd_mip0)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, rw_spd_mip1)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, rw_spd_mip2)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, rw_spd_mip3)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, rw_spd_mip4)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, rw_spd_mip5)
	END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FArenaFSR3ShadingChangePyramidCS, "/Plugin/ArenaUpscaling/Private/ArenaFSR3Upscaler.usf", "MainCS", SF_Compute);

class FArenaFSR3ShadingChangeCS : public FGlobalShader
{
	ARENA_FSR3_SHADER_BODY(FArenaFSR3ShadingChangeCS, ArenaFSR3::PassShadingChange)
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FArenaFSR3Constants, Constants)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, r_spd_mips)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, rw_shading_change)
	END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FArenaFSR3ShadingChangeCS, "/Plugin/ArenaUpscaling/Private/ArenaFSR3Upscaler.usf", "MainCS", SF_Compute);

class FArenaFSR3PrepareReactivityCS : public FGlobalShader
{
	ARENA_FSR3_SHADER_BODY(FArenaFSR3PrepareReactivityCS, ArenaFSR3::PassPrepareReactivity)
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FArenaFSR3Constants, Constants)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, r_reconstructed_previous_nearest_depth)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, r_dilated_motion_vectors)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, r_dilated_depth)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, r_reactive_mask)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, r_transparency_and_composition_mask)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, r_accumulation)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, r_shading_change)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, r_current_luma)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, r_input_exposure)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, rw_dilated_reactive_masks)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, rw_new_locks)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, rw_accumulation)
	END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FArenaFSR3PrepareReactivityCS, "/Plugin/ArenaUpscaling/Private/ArenaFSR3Upscaler.usf", "MainCS", SF_Compute);

class FArenaFSR3LumaInstabilityCS : public FGlobalShader
{
	ARENA_FSR3_SHADER_BODY(FArenaFSR3LumaInstabilityCS, ArenaFSR3::PassLumaInstability)
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FArenaFSR3Constants, Constants)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, r_input_exposure)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, r_dilated_reactive_masks)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, r_dilated_motion_vectors)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, r_frame_info)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, r_luma_history)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, r_farthest_depth_mip1)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, r_current_luma)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, rw_luma_history)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, rw_luma_instability)
	END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FArenaFSR3LumaInstabilityCS, "/Plugin/ArenaUpscaling/Private/ArenaFSR3Upscaler.usf", "MainCS", SF_Compute);

class FArenaFSR3AccumulateCS : public FGlobalShader
{
	ARENA_FSR3_SHADER_BODY(FArenaFSR3AccumulateCS, ArenaFSR3::PassAccumulate)
	class FSharpenDim : SHADER_PERMUTATION_BOOL("ARENA_FSR3_SHARPEN");
	using FPermutationDomain = TShaderPermutationDomain<FSharpenDim>;
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FArenaFSR3Constants, Constants)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, r_input_exposure)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, r_dilated_reactive_masks)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, r_dilated_motion_vectors)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, r_internal_upscaled_color)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, r_farthest_depth_mip1)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, r_current_luma)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, r_luma_instability)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, r_input_color_jittered)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, rw_internal_upscaled_color)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, rw_upscaled_output)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, rw_new_locks)
	END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FArenaFSR3AccumulateCS, "/Plugin/ArenaUpscaling/Private/ArenaFSR3Upscaler.usf", "MainCS", SF_Compute);

class FArenaFSR3RcasCS : public FGlobalShader
{
	ARENA_FSR3_SHADER_BODY(FArenaFSR3RcasCS, ArenaFSR3::PassRcas)
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FArenaFSR3Constants, Constants)
		SHADER_PARAMETER(FUintVector4, rcasConfig)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, r_input_exposure)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, r_rcas_input)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, rw_upscaled_output)
	END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FArenaFSR3RcasCS, "/Plugin/ArenaUpscaling/Private/ArenaFSR3Upscaler.usf", "MainCS", SF_Compute);

#undef ARENA_FSR3_SHADER_BODY

// ---------------------------------------------------------------------------------------------------------------------

uint64 FArenaFSR3Upscaler::FHistory::GetGPUSizeBytes() const
{
	uint64 Size = 0;
	for (const TRefCountPtr<IPooledRenderTarget>* RT : { &Accumulation, &Luma, &LumaHistory, &InternalUpscaled, &FrameInfo })
	{
		if (RT->IsValid())
		{
			Size += (*RT)->ComputeMemorySize();
		}
	}
	return Size;
}

namespace ArenaFSR3
{
	static FRDGTextureRef CreateTexture(FRDGBuilder& GraphBuilder, FIntPoint Extent, EPixelFormat Format, const TCHAR* Name, uint8 NumMips = 1)
	{
		const FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(FIntPoint(FMath::Max(Extent.X, 1), FMath::Max(Extent.Y, 1)), Format, FClearValueBinding::Black,
			TexCreate_ShaderResource | TexCreate_UAV, NumMips);
		return GraphBuilder.CreateTexture(Desc, Name);
	}

	/** The history texture from the previous frame, or a cleared stand-in when there is none (first frame, reset). */
	static FRDGTextureRef PreviousOrCleared(FRDGBuilder& GraphBuilder, const TRefCountPtr<IPooledRenderTarget>& Previous, bool bReset,
		FIntPoint Extent, EPixelFormat Format, const TCHAR* Name, const FVector4f& ClearValue = FVector4f::Zero())
	{
		if (!bReset && Previous.IsValid())
		{
			return GraphBuilder.RegisterExternalTexture(Previous, Name);
		}
		FRDGTextureRef Texture = CreateTexture(GraphBuilder, Extent, Format, Name);
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Texture), FVector4(ClearValue));
		return Texture;
	}

	static int32 JitterPhaseCount(int32 RenderWidth, int32 DisplayWidth)
	{
		return int32(8.f * FMath::Square(float(DisplayWidth) / float(FMath::Max(RenderWidth, 1))));
	}

	template <typename ShaderType>
	static void AddPass(FRDGBuilder& GraphBuilder, const FGlobalShaderMap* ShaderMap, FRDGEventName&& Name, typename ShaderType::FParameters* Parameters,
		FIntVector Groups, typename ShaderType::FPermutationDomain Permutation = typename ShaderType::FPermutationDomain())
	{
		TShaderMapRef<ShaderType> Shader(ShaderMap, Permutation);
		FComputeShaderUtils::AddPass(GraphBuilder, MoveTemp(Name), ERDGPassFlags::Compute, Shader, Parameters, Groups);
	}
}

UE::Renderer::Private::ITemporalUpscaler::FOutputs FArenaFSR3Upscaler::AddPasses(FRDGBuilder& GraphBuilder, const FSceneView& View, const FInputs& Inputs) const
{
	using namespace ArenaFSR3;
	RDG_EVENT_SCOPE(GraphBuilder, "FSR 3.1");

	const FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(View.GetFeatureLevel());
	const FHistory* Prev = Inputs.PrevHistory.IsValid() ? static_cast<const FHistory*>(Inputs.PrevHistory.GetReference()) : nullptr;

	const FIntRect InputRect = Inputs.SceneColor.ViewRect;
	const FIntPoint RenderSize = InputRect.Size();
	const FIntPoint UpscaleSize = Inputs.OutputViewRect.Size();
	// Internal render-resolution textures use the scene texture extent so a changing screen percentage keeps them
	// (FSR handles a render size below its "max render size"); the upscaled ones use the output size.
	const FIntPoint RenderExtent = Inputs.SceneColor.Texture->Desc.Extent;
	const FIntPoint UpscaleExtent = UpscaleSize;
	const FIntPoint HalfExtent(FMath::Max(RenderExtent.X / 2, 1), FMath::Max(RenderExtent.Y / 2, 1));

	const bool bReset = Prev == nullptr || View.bCameraCut || Prev->RenderExtent != RenderExtent || Prev->UpscaleExtent != UpscaleExtent;

	// ---- constants (ffx_fsr3upscaler.cpp: fsr3upscalerDispatch) ----
	FArenaFSR3Constants Constants;
	Constants.iRenderSize = RenderSize;
	Constants.iPreviousFrameRenderSize = bReset ? RenderSize : Prev->RenderSize;
	Constants.iUpscaleSize = UpscaleSize;
	Constants.iPreviousFrameUpscaleSize = bReset ? UpscaleSize : Prev->UpscaleSize;
	Constants.iMaxRenderSize = RenderExtent;
	Constants.iMaxUpscaleSize = UpscaleExtent;
	{
		// Reversed, infinite far plane (Unreal): view depth = near / device depth.
		const FMatrix& Proj = View.ViewMatrices.GetViewToClip();
		const float Near = View.NearClippingDistance;
		Constants.fDeviceToViewDepth = FVector4f(-FLT_EPSILON, Near, 1.f / float(Proj.M[0][0]), 1.f / float(Proj.M[1][1]));
		Constants.fTanHalfFOV = 1.f / float(Proj.M[0][0]);
	}
	Constants.fJitter = Inputs.TemporalJitterPixels;
	Constants.fPreviousFrameJitter = bReset ? Inputs.TemporalJitterPixels : Prev->Jitter;
	// ArenaFSR3Inputs.usf writes motion vectors directly as UV offsets.
	Constants.fMotionVectorScale = FVector2f(1.f, 1.f);
	Constants.fDownscaleFactor = FVector2f(float(RenderSize.X) / float(UpscaleSize.X), float(RenderSize.Y) / float(UpscaleSize.Y));
	Constants.fMotionVectorJitterCancellation = FVector2f::ZeroVector;
	{
		const int32 PhaseCount = JitterPhaseCount(RenderSize.X, UpscaleSize.X);
		float Phase = (bReset || Prev->JitterPhaseCount <= 0.f) ? float(PhaseCount) : Prev->JitterPhaseCount;
		if (!bReset)
		{
			Phase += float(FMath::Sign(PhaseCount - int32(Phase)));
		}
		Constants.fJitterSequenceLength = Phase;
	}
	Constants.fDeltaTime = FMath::Clamp(Settings.DeltaTimeSeconds, 0.f, 1.f);
	const float PreExposure = Inputs.PreExposure > 0.f ? Inputs.PreExposure : 1.f;
	Constants.fDeltaPreExposure = (!bReset && Prev->PreExposure > 0.f) ? PreExposure / Prev->PreExposure : 1.f;
	Constants.fViewSpaceToMetersFactor = 0.01f; // Unreal units are centimetres
	Constants.fFrameIndex = bReset ? 0.f : Prev->FrameIndex + 1.f;
	Constants.fVelocityFactor = 1.f;
	Constants.fReactivenessScale = 1.f;
	Constants.fShadingChangeScale = 1.f;
	Constants.fAccumulationAddedPerFrame = 1.f / 3.f;
	Constants.fMinDisocclusionAccumulation = -1.f / 3.f;
	Constants.s_PointClamp = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	Constants.s_LinearClamp = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

	FArenaFSR3SpdConstants Spd;
	FIntVector SpdGroups;
	{
		// ffxSpdSetup with rect (0, 0, RenderSize)
		const uint32 EndX = (RenderSize.X - 1) / 64;
		const uint32 EndY = (RenderSize.Y - 1) / 64;
		SpdGroups = FIntVector(EndX + 1, EndY + 1, 1);
		Spd.numWorkGroups = uint32(SpdGroups.X * SpdGroups.Y);
		Spd.mips = uint32(FMath::Min(FMath::FloorToFloat(FMath::Log2(float(FMath::Max(RenderSize.X, RenderSize.Y)))), 12.f));
		Spd.workGroupOffset = FUintVector2(0, 0);
		Spd.renderSize = FUintVector2(uint32(RenderSize.X), uint32(RenderSize.Y));
	}

	// ---- inputs ----
	FRDGTextureRef MotionVectors = CreateTexture(GraphBuilder, RenderExtent, PF_G16R16F, TEXT("FSR3.MotionVectors"));
	FRDGTextureRef Depth = CreateTexture(GraphBuilder, RenderExtent, PF_R32_FLOAT, TEXT("FSR3.Depth"));
	FRDGTextureRef Exposure = CreateTexture(GraphBuilder, FIntPoint(1, 1), PF_G32R32F, TEXT("FSR3.Exposure"));
	{
		FArenaFSR3InputsCS::FParameters* P = GraphBuilder.AllocParameters<FArenaFSR3InputsCS::FParameters>();
		P->View = View.ViewUniformBuffer;
		P->VelocityTexture = Inputs.SceneVelocity.Texture;
		P->DepthTexture = Inputs.SceneDepth.Texture;
		P->EyeAdaptationTexture = Inputs.EyeAdaptationTexture;
		P->InputViewMin = InputRect.Min;
		P->InputViewSize = RenderSize;
		P->OneOverPreExposure = 1.f / PreExposure;
		P->OutMotionVectors = GraphBuilder.CreateUAV(MotionVectors);
		P->OutDepth = GraphBuilder.CreateUAV(Depth);
		P->OutExposure = GraphBuilder.CreateUAV(Exposure);
		TShaderMapRef<FArenaFSR3InputsCS> Shader(ShaderMap);
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("ConvertInputs"), ERDGPassFlags::Compute, Shader, P,
			FComputeShaderUtils::GetGroupCount(RenderSize, FIntPoint(8, 8)));
	}

	// FSR loads the color by pixel position from (0,0): copy it when the view does not start at the texture origin.
	FRDGTextureRef Color = Inputs.SceneColor.Texture;
	if (InputRect.Min != FIntPoint::ZeroValue)
	{
		Color = CreateTexture(GraphBuilder, RenderExtent, Inputs.SceneColor.Texture->Desc.Format, TEXT("FSR3.Color"));
		FRHICopyTextureInfo Copy;
		Copy.SourcePosition = FIntVector(InputRect.Min.X, InputRect.Min.Y, 0);
		Copy.Size = FIntVector(RenderSize.X, RenderSize.Y, 1);
		AddCopyTexturePass(GraphBuilder, Inputs.SceneColor.Texture, Color, Copy);
	}

	// ---- persistent resources (ping-pong across frames through the history) ----
	FRDGTextureRef PrevAccumulation = PreviousOrCleared(GraphBuilder, Prev ? Prev->Accumulation : nullptr, bReset, RenderExtent, PF_R8, TEXT("FSR3.Accumulation.Prev"));
	FRDGTextureRef PrevLuma = PreviousOrCleared(GraphBuilder, Prev ? Prev->Luma : nullptr, bReset, RenderExtent, PF_R16F, TEXT("FSR3.Luma.Prev"));
	FRDGTextureRef PrevLumaHistory = PreviousOrCleared(GraphBuilder, Prev ? Prev->LumaHistory : nullptr, bReset, RenderExtent, PF_FloatRGBA, TEXT("FSR3.LumaHistory.Prev"));
	FRDGTextureRef PrevUpscaled = PreviousOrCleared(GraphBuilder, Prev ? Prev->InternalUpscaled : nullptr, bReset, UpscaleExtent, PF_FloatRGBA, TEXT("FSR3.InternalUpscaled.Prev"));
	// Frame info is read and written by the luma pyramid in place: carry the same texture over.
	FRDGTextureRef FrameInfo = PreviousOrCleared(GraphBuilder, Prev ? Prev->FrameInfo : nullptr, bReset, FIntPoint(1, 1), PF_A32B32G32R32F, TEXT("FSR3.FrameInfo"), FVector4f(-1.f, 1.f, 0.f, 0.f));

	FRDGTextureRef NewAccumulation = CreateTexture(GraphBuilder, RenderExtent, PF_R8, TEXT("FSR3.Accumulation"));
	FRDGTextureRef CurrentLuma = CreateTexture(GraphBuilder, RenderExtent, PF_R16F, TEXT("FSR3.Luma"));
	FRDGTextureRef NewLumaHistory = CreateTexture(GraphBuilder, RenderExtent, PF_FloatRGBA, TEXT("FSR3.LumaHistory"));
	FRDGTextureRef NewUpscaled = CreateTexture(GraphBuilder, UpscaleExtent, PF_FloatRGBA, TEXT("FSR3.InternalUpscaled"));

	// ---- per-frame intermediates ----
	FRDGTextureRef DilatedMotionVectors = CreateTexture(GraphBuilder, RenderExtent, PF_G16R16F, TEXT("FSR3.DilatedMotionVectors"));
	FRDGTextureRef DilatedDepth = CreateTexture(GraphBuilder, RenderExtent, PF_R32_FLOAT, TEXT("FSR3.DilatedDepth"));
	FRDGTextureRef ReconstructedPrevDepth = CreateTexture(GraphBuilder, RenderExtent, PF_R32_UINT, TEXT("FSR3.ReconstructedPrevNearestDepth"));
	FRDGTextureRef FarthestDepth = CreateTexture(GraphBuilder, RenderExtent, PF_R16F, TEXT("FSR3.FarthestDepth"));
	FRDGTextureRef FarthestDepthMip1 = CreateTexture(GraphBuilder, HalfExtent, PF_R16F, TEXT("FSR3.FarthestDepthMip1"));
	FRDGTextureRef LumaInstability = CreateTexture(GraphBuilder, RenderExtent, PF_R16F, TEXT("FSR3.LumaInstability"));
	FRDGTextureRef SpdMips = CreateTexture(GraphBuilder, HalfExtent, PF_G16R16F, TEXT("FSR3.SpdMips"), 6);
	FRDGTextureRef SpdAtomic = CreateTexture(GraphBuilder, FIntPoint(1, 1), PF_R32_UINT, TEXT("FSR3.SpdAtomic"));
	FRDGTextureRef ShadingChange = CreateTexture(GraphBuilder, HalfExtent, PF_R8, TEXT("FSR3.ShadingChange"));
	FRDGTextureRef DilatedReactiveMasks = CreateTexture(GraphBuilder, RenderExtent, PF_R8G8B8A8, TEXT("FSR3.DilatedReactiveMasks"));
	FRDGTextureRef NewLocks = CreateTexture(GraphBuilder, UpscaleExtent, PF_R8, TEXT("FSR3.NewLocks"));
	// No reactive or transparency/composition mask from the renderer: FSR's default is an all-zero mask.
	FRDGTextureRef ZeroMask = CreateTexture(GraphBuilder, RenderExtent, PF_R8, TEXT("FSR3.ZeroMask"));

	FRDGTextureRef Output = CreateTexture(GraphBuilder, UpscaleExtent, PF_FloatRGBA, TEXT("FSR3.Output"));

	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(ReconstructedPrevDepth), uint32(0)); // inverted depth: farthest = 0
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SpdAtomic), uint32(0));
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(NewLocks), 0.f);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(ZeroMask), 0.f);
	for (uint8 Mip = 0; Mip < 6; ++Mip)
	{
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGTextureUAVDesc(SpdMips, Mip)), FVector2D::ZeroVector);
	}

	const FIntVector SrcGroups = FComputeShaderUtils::GetGroupCount(RenderSize, FIntPoint(8, 8));
	const FIntVector DstGroups = FComputeShaderUtils::GetGroupCount(UpscaleSize, FIntPoint(8, 8));
	const FIntVector ShadingChangeGroups = FComputeShaderUtils::GetGroupCount(FIntPoint(int32(RenderSize.X * 0.5f), int32(RenderSize.Y * 0.5f)), FIntPoint(8, 8));

	{
		FArenaFSR3PrepareInputsCS::FParameters* P = GraphBuilder.AllocParameters<FArenaFSR3PrepareInputsCS::FParameters>();
		P->Constants = Constants;
		P->r_input_motion_vectors = MotionVectors;
		P->r_input_depth = Depth;
		P->r_input_color_jittered = Color;
		P->rw_dilated_motion_vectors = GraphBuilder.CreateUAV(DilatedMotionVectors);
		P->rw_dilated_depth = GraphBuilder.CreateUAV(DilatedDepth);
		P->rw_reconstructed_previous_nearest_depth = GraphBuilder.CreateUAV(ReconstructedPrevDepth);
		P->rw_farthest_depth = GraphBuilder.CreateUAV(FarthestDepth);
		P->rw_current_luma = GraphBuilder.CreateUAV(CurrentLuma);
		AddPass<FArenaFSR3PrepareInputsCS>(GraphBuilder, ShaderMap, RDG_EVENT_NAME("PrepareInputs"), P, SrcGroups);
	}
	{
		FArenaFSR3LumaPyramidCS::FParameters* P = GraphBuilder.AllocParameters<FArenaFSR3LumaPyramidCS::FParameters>();
		P->Constants = Constants;
		P->Spd = Spd;
		P->r_current_luma = CurrentLuma;
		P->r_farthest_depth = FarthestDepth;
		P->rw_spd_global_atomic = GraphBuilder.CreateUAV(SpdAtomic);
		P->rw_frame_info = GraphBuilder.CreateUAV(FrameInfo);
		P->rw_spd_mip0 = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(SpdMips, 0));
		P->rw_spd_mip1 = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(SpdMips, 1));
		P->rw_spd_mip2 = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(SpdMips, 2));
		P->rw_spd_mip3 = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(SpdMips, 3));
		P->rw_spd_mip4 = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(SpdMips, 4));
		P->rw_spd_mip5 = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(SpdMips, 5));
		P->rw_farthest_depth_mip1 = GraphBuilder.CreateUAV(FarthestDepthMip1);
		AddPass<FArenaFSR3LumaPyramidCS>(GraphBuilder, ShaderMap, RDG_EVENT_NAME("LumaPyramid"), P, SpdGroups);
	}
	{
		FArenaFSR3ShadingChangePyramidCS::FParameters* P = GraphBuilder.AllocParameters<FArenaFSR3ShadingChangePyramidCS::FParameters>();
		P->Constants = Constants;
		P->Spd = Spd;
		P->r_current_luma = CurrentLuma;
		P->r_previous_luma = PrevLuma;
		P->r_dilated_motion_vectors = DilatedMotionVectors;
		P->r_input_exposure = Exposure;
		P->rw_spd_global_atomic = GraphBuilder.CreateUAV(SpdAtomic);
		P->rw_spd_mip0 = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(SpdMips, 0));
		P->rw_spd_mip1 = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(SpdMips, 1));
		P->rw_spd_mip2 = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(SpdMips, 2));
		P->rw_spd_mip3 = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(SpdMips, 3));
		P->rw_spd_mip4 = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(SpdMips, 4));
		P->rw_spd_mip5 = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(SpdMips, 5));
		AddPass<FArenaFSR3ShadingChangePyramidCS>(GraphBuilder, ShaderMap, RDG_EVENT_NAME("ShadingChangePyramid"), P, SpdGroups);
	}
	{
		FArenaFSR3ShadingChangeCS::FParameters* P = GraphBuilder.AllocParameters<FArenaFSR3ShadingChangeCS::FParameters>();
		P->Constants = Constants;
		P->r_spd_mips = SpdMips;
		P->rw_shading_change = GraphBuilder.CreateUAV(ShadingChange);
		AddPass<FArenaFSR3ShadingChangeCS>(GraphBuilder, ShaderMap, RDG_EVENT_NAME("ShadingChange"), P, ShadingChangeGroups);
	}
	{
		FArenaFSR3PrepareReactivityCS::FParameters* P = GraphBuilder.AllocParameters<FArenaFSR3PrepareReactivityCS::FParameters>();
		P->Constants = Constants;
		P->r_reconstructed_previous_nearest_depth = ReconstructedPrevDepth;
		P->r_dilated_motion_vectors = DilatedMotionVectors;
		P->r_dilated_depth = DilatedDepth;
		P->r_reactive_mask = ZeroMask;
		P->r_transparency_and_composition_mask = ZeroMask;
		P->r_accumulation = PrevAccumulation;
		P->r_shading_change = ShadingChange;
		P->r_current_luma = CurrentLuma;
		P->r_input_exposure = Exposure;
		P->rw_dilated_reactive_masks = GraphBuilder.CreateUAV(DilatedReactiveMasks);
		P->rw_new_locks = GraphBuilder.CreateUAV(NewLocks);
		P->rw_accumulation = GraphBuilder.CreateUAV(NewAccumulation);
		AddPass<FArenaFSR3PrepareReactivityCS>(GraphBuilder, ShaderMap, RDG_EVENT_NAME("PrepareReactivity"), P, SrcGroups);
	}
	{
		FArenaFSR3LumaInstabilityCS::FParameters* P = GraphBuilder.AllocParameters<FArenaFSR3LumaInstabilityCS::FParameters>();
		P->Constants = Constants;
		P->r_input_exposure = Exposure;
		P->r_dilated_reactive_masks = DilatedReactiveMasks;
		P->r_dilated_motion_vectors = DilatedMotionVectors;
		P->r_frame_info = FrameInfo;
		P->r_luma_history = PrevLumaHistory;
		P->r_farthest_depth_mip1 = FarthestDepthMip1;
		P->r_current_luma = CurrentLuma;
		P->rw_luma_history = GraphBuilder.CreateUAV(NewLumaHistory);
		P->rw_luma_instability = GraphBuilder.CreateUAV(LumaInstability);
		AddPass<FArenaFSR3LumaInstabilityCS>(GraphBuilder, ShaderMap, RDG_EVENT_NAME("LumaInstability"), P, SrcGroups);
	}
	const bool bSharpen = Settings.Sharpness > 0.f;
	{
		FArenaFSR3AccumulateCS::FParameters* P = GraphBuilder.AllocParameters<FArenaFSR3AccumulateCS::FParameters>();
		P->Constants = Constants;
		P->r_input_exposure = Exposure;
		P->r_dilated_reactive_masks = DilatedReactiveMasks;
		P->r_dilated_motion_vectors = DilatedMotionVectors;
		P->r_internal_upscaled_color = PrevUpscaled;
		P->r_farthest_depth_mip1 = FarthestDepthMip1;
		P->r_current_luma = CurrentLuma;
		P->r_luma_instability = LumaInstability;
		P->r_input_color_jittered = Color;
		P->rw_internal_upscaled_color = GraphBuilder.CreateUAV(NewUpscaled);
		P->rw_upscaled_output = GraphBuilder.CreateUAV(Output);
		P->rw_new_locks = GraphBuilder.CreateUAV(NewLocks);
		FArenaFSR3AccumulateCS::FPermutationDomain Permutation;
		Permutation.Set<FArenaFSR3AccumulateCS::FSharpenDim>(bSharpen);
		AddPass<FArenaFSR3AccumulateCS>(GraphBuilder, ShaderMap, RDG_EVENT_NAME("Accumulate%s", bSharpen ? TEXT(" (sharpen)") : TEXT("")), P, DstGroups, Permutation);
	}
	if (bSharpen)
	{
		FArenaFSR3RcasCS::FParameters* P = GraphBuilder.AllocParameters<FArenaFSR3RcasCS::FParameters>();
		P->Constants = Constants;
		// FsrRcasCon: sharpness in stops, 0 = maximum. The SDK maps its 0..1 sharpness to 2..0 stops.
		const float Stops = -2.f * FMath::Clamp(Settings.Sharpness, 0.f, 1.f) + 2.f;
		const float Linear = FMath::Exp2(-Stops);
		const FFloat16 Half(Linear);
		uint32 LinearBits = 0;
		FMemory::Memcpy(&LinearBits, &Linear, sizeof(LinearBits));
		P->rcasConfig = FUintVector4(LinearBits, uint32(Half.Encoded) | (uint32(Half.Encoded) << 16), 0, 0);
		P->r_input_exposure = Exposure;
		P->r_rcas_input = NewUpscaled;
		P->rw_upscaled_output = GraphBuilder.CreateUAV(Output);
		AddPass<FArenaFSR3RcasCS>(GraphBuilder, ShaderMap, RDG_EVENT_NAME("RCAS"), P, FComputeShaderUtils::GetGroupCount(UpscaleSize, FIntPoint(16, 16)));
	}

	// ---- history for the next frame ----
	FHistory* NewHistory = new FHistory();
	GraphBuilder.QueueTextureExtraction(NewAccumulation, &NewHistory->Accumulation);
	GraphBuilder.QueueTextureExtraction(CurrentLuma, &NewHistory->Luma);
	GraphBuilder.QueueTextureExtraction(NewLumaHistory, &NewHistory->LumaHistory);
	GraphBuilder.QueueTextureExtraction(NewUpscaled, &NewHistory->InternalUpscaled);
	GraphBuilder.QueueTextureExtraction(FrameInfo, &NewHistory->FrameInfo);
	NewHistory->RenderExtent = RenderExtent;
	NewHistory->UpscaleExtent = UpscaleExtent;
	NewHistory->RenderSize = RenderSize;
	NewHistory->UpscaleSize = UpscaleSize;
	NewHistory->Jitter = Inputs.TemporalJitterPixels;
	NewHistory->PreExposure = PreExposure;
	NewHistory->FrameIndex = Constants.fFrameIndex;
	NewHistory->JitterPhaseCount = Constants.fJitterSequenceLength;

	FOutputs Outputs;
	Outputs.FullRes = FScreenPassTexture(Output, Inputs.OutputViewRect);
	Outputs.NewHistory = NewHistory;
	return Outputs;
}
