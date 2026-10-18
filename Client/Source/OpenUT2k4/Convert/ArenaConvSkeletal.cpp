#include "ArenaConvConfig.h"
#if ARENA_CONV_BUILD
// SkeletalMesh / MeshAnimation / VertMesh export (port of Tools/ut2k4/skeletal.py, skelexport.py and the
// matching parts of exporter.py). Everything is computed in double precision like the numpy original.

#include "ArenaConvExporter.h"
#include "ArenaConvGltf.h"
#include "Misc/Paths.h"
#include "Algo/StableSort.h"
#include "Algo/Unique.h"

namespace ArenaConvSkel
{
	constexpr int32 ToolVersion = 1;
	constexpr int32 AnimToolVersion = 2;

	// ---- small linear algebra (column-vector convention, M[row][col]) ----
	struct FM3
	{
		double M[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
		static FM3 Zero() { FM3 R; for (auto& Row : R.M) for (double& V : Row) V = 0; return R; }
		FM3 operator*(const FM3& B) const
		{
			FM3 R = Zero();
			for (int32 i = 0; i < 3; ++i) for (int32 j = 0; j < 3; ++j) for (int32 k = 0; k < 3; ++k) R.M[i][j] += M[i][k] * B.M[k][j];
			return R;
		}
		void Mul(const double V[3], double Out[3]) const
		{
			for (int32 i = 0; i < 3; ++i) Out[i] = M[i][0] * V[0] + M[i][1] * V[1] + M[i][2] * V[2];
		}
		double Det() const
		{
			return M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) + M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
		}
		FM3 Transposed() const { FM3 R; for (int32 i = 0; i < 3; ++i) for (int32 j = 0; j < 3; ++j) R.M[i][j] = M[j][i]; return R; }
		FM3 Inverse() const
		{
			const double D = Det();
			FM3 R;
			const double I = D != 0 ? 1.0 / D : 0.0;
			R.M[0][0] = (M[1][1] * M[2][2] - M[1][2] * M[2][1]) * I;
			R.M[0][1] = (M[0][2] * M[2][1] - M[0][1] * M[2][2]) * I;
			R.M[0][2] = (M[0][1] * M[1][2] - M[0][2] * M[1][1]) * I;
			R.M[1][0] = (M[1][2] * M[2][0] - M[1][0] * M[2][2]) * I;
			R.M[1][1] = (M[0][0] * M[2][2] - M[0][2] * M[2][0]) * I;
			R.M[1][2] = (M[0][2] * M[1][0] - M[0][0] * M[1][2]) * I;
			R.M[2][0] = (M[1][0] * M[2][1] - M[1][1] * M[2][0]) * I;
			R.M[2][1] = (M[0][1] * M[2][0] - M[0][0] * M[2][1]) * I;
			R.M[2][2] = (M[0][0] * M[1][1] - M[0][1] * M[1][0]) * I;
			return R;
		}
	};

	struct FM4
	{
		FM3 R;
		double T[3] = { 0, 0, 0 };
		FM4 operator*(const FM4& B) const
		{
			FM4 Out;
			Out.R = R * B.R;
			R.Mul(B.T, Out.T);
			for (int32 i = 0; i < 3; ++i) Out.T[i] += T[i];
			return Out;
		}
		FM4 Inverse() const
		{
			FM4 Out;
			Out.R = R.Inverse();
			double NT[3];
			Out.R.Mul(T, NT);
			for (int32 i = 0; i < 3; ++i) Out.T[i] = -NT[i];
			return Out;
		}
		FMatrix44d ToUE() const
		{
			FMatrix44d O;
			for (int32 r = 0; r < 3; ++r)
			{
				for (int32 c = 0; c < 3; ++c) O.M[r][c] = R.M[r][c];
				O.M[r][3] = T[r];
			}
			O.M[3][0] = O.M[3][1] = O.M[3][2] = 0;
			O.M[3][3] = 1;
			return O;
		}
	};

	struct FQ { double X = 0, Y = 0, Z = 0, W = 1; };

	FQ QNorm(FQ Q)
	{
		double N = FMath::Sqrt(Q.X * Q.X + Q.Y * Q.Y + Q.Z * Q.Z + Q.W * Q.W);
		if (N < 1e-12) N = 1.0;
		return { Q.X / N, Q.Y / N, Q.Z / N, Q.W / N };
	}
	FQ QConj(FQ Q) { return { -Q.X, -Q.Y, -Q.Z, Q.W }; }
	FQ QMul(const FQ& A, const FQ& B)
	{
		return { A.W * B.X + A.X * B.W + A.Y * B.Z - A.Z * B.Y,
			A.W * B.Y - A.X * B.Z + A.Y * B.W + A.Z * B.X,
			A.W * B.Z + A.X * B.Y - A.Y * B.X + A.Z * B.W,
			A.W * B.W - A.X * B.X - A.Y * B.Y - A.Z * B.Z };
	}
	double QDot(const FQ& A, const FQ& B) { return A.X * B.X + A.Y * B.Y + A.Z * B.Z + A.W * B.W; }

	FM3 QuatToMatrix(FQ Q)
	{
		Q = QNorm(Q);
		const double x = Q.X, y = Q.Y, z = Q.Z, w = Q.W;
		FM3 R;
		R.M[0][0] = 1 - 2 * (y * y + z * z); R.M[0][1] = 2 * (x * y - z * w); R.M[0][2] = 2 * (x * z + y * w);
		R.M[1][0] = 2 * (x * y + z * w); R.M[1][1] = 1 - 2 * (x * x + z * z); R.M[1][2] = 2 * (y * z - x * w);
		R.M[2][0] = 2 * (x * z - y * w); R.M[2][1] = 2 * (y * z + x * w); R.M[2][2] = 1 - 2 * (x * x + y * y);
		return R;
	}

	/** skelexport.mat_to_quat; false when the matrix makes a negative square root (Python raises). */
	bool MatToQuat(const FM3& Mx, FQ& Out)
	{
		const auto& m = Mx.M;
		const double Tr = m[0][0] + m[1][1] + m[2][2];
		double q[4];
		if (Tr > 0)
		{
			const double S = FMath::Sqrt(Tr + 1.0) * 2;
			q[0] = (m[2][1] - m[1][2]) / S; q[1] = (m[0][2] - m[2][0]) / S; q[2] = (m[1][0] - m[0][1]) / S; q[3] = 0.25 * S;
		}
		else if (m[0][0] > m[1][1] && m[0][0] > m[2][2])
		{
			const double A = 1.0 + m[0][0] - m[1][1] - m[2][2];
			if (A < 0) return false;
			const double S = FMath::Sqrt(A) * 2;
			q[0] = 0.25 * S; q[1] = (m[0][1] + m[1][0]) / S; q[2] = (m[0][2] + m[2][0]) / S; q[3] = (m[2][1] - m[1][2]) / S;
		}
		else if (m[1][1] > m[2][2])
		{
			const double A = 1.0 + m[1][1] - m[0][0] - m[2][2];
			if (A < 0) return false;
			const double S = FMath::Sqrt(A) * 2;
			q[0] = (m[0][1] + m[1][0]) / S; q[1] = 0.25 * S; q[2] = (m[1][2] + m[2][1]) / S; q[3] = (m[0][2] - m[2][0]) / S;
		}
		else
		{
			const double A = 1.0 + m[2][2] - m[0][0] - m[1][1];
			if (A < 0) return false;
			const double S = FMath::Sqrt(A) * 2;
			q[0] = (m[0][2] + m[2][0]) / S; q[1] = (m[1][2] + m[2][1]) / S; q[2] = 0.25 * S; q[3] = (m[1][0] - m[0][1]) / S;
		}
		if (q[3] < 0)
		{
			for (double& V : q) V = -V;
		}
		const double N = FMath::Sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
		Out = { q[0] / N, q[1] / N, q[2] / N, q[3] / N };
		return true;
	}

	/** skelexport.rotator_matrix: columns are the X/Y/Z axes. */
	FM3 RotatorMatrix(double Pitch, double Yaw, double Roll)
	{
		const double p = Pitch * 2.0 * PI / 65536.0, y = Yaw * 2.0 * PI / 65536.0, r = Roll * 2.0 * PI / 65536.0;
		const double sp = FMath::Sin(p), cp = FMath::Cos(p), sy = FMath::Sin(y), cy = FMath::Cos(y), sr = FMath::Sin(r), cr = FMath::Cos(r);
		FM3 Rows;
		Rows.M[0][0] = cp * cy; Rows.M[0][1] = cp * sy; Rows.M[0][2] = sp;
		Rows.M[1][0] = sr * sp * cy - cr * sy; Rows.M[1][1] = sr * sp * sy + cr * cy; Rows.M[1][2] = -sr * cp;
		Rows.M[2][0] = -(cr * sp * cy + sr * sy); Rows.M[2][1] = cy * sr - cr * sp * sy; Rows.M[2][2] = cr * cp;
		return Rows.Transposed();
	}

	FConvJson QuatToRotator(const FQ& Q)
	{
		const FM3 M = QuatToMatrix(Q);
		const double X[3] = { M.M[0][0], M.M[1][0], M.M[2][0] };
		const double Y[3] = { M.M[0][1], M.M[1][1], M.M[2][1] };
		const double Z[3] = { M.M[0][2], M.M[1][2], M.M[2][2] };
		const double Pitch = FMath::Atan2(X[2], FMath::Sqrt(X[0] * X[0] + X[1] * X[1]));
		const double Yaw = FMath::Atan2(X[1], X[0]);
		const FM3 Sm = RotatorMatrix(Pitch * 32768 / PI, Yaw * 32768 / PI, 0);
		const double Sy[3] = { Sm.M[0][1], Sm.M[1][1], Sm.M[2][1] };
		const double Roll = FMath::Atan2(Z[0] * Sy[0] + Z[1] * Sy[1] + Z[2] * Sy[2], Y[0] * Sy[0] + Y[1] * Sy[1] + Y[2] * Sy[2]);
		FConvJson O = FConvJson::Object();
		O.Set(TEXT("pitch"), FConvJson::RoundTo(FMath::RadiansToDegrees(Pitch), 4));
		O.Set(TEXT("yaw"), FConvJson::RoundTo(FMath::RadiansToDegrees(Yaw), 4));
		O.Set(TEXT("roll"), FConvJson::RoundTo(FMath::RadiansToDegrees(Roll), 4));
		return O;
	}

	/** Symmetric 3x3 eigen decomposition (Jacobi). Eigenvalues descending, eigenvectors as columns of V. */
	void SymEigen(FM3 A, double Eval[3], FM3& V)
	{
		V = FM3();
		for (int32 Sweep = 0; Sweep < 64; ++Sweep)
		{
			const double Off = FMath::Abs(A.M[0][1]) + FMath::Abs(A.M[0][2]) + FMath::Abs(A.M[1][2]);
			if (Off < 1e-300)
			{
				break;
			}
			for (int32 p = 0; p < 2; ++p)
			{
				for (int32 q = p + 1; q < 3; ++q)
				{
					if (FMath::Abs(A.M[p][q]) < 1e-300)
					{
						continue;
					}
					const double Theta = (A.M[q][q] - A.M[p][p]) / (2 * A.M[p][q]);
					const double T = (Theta >= 0 ? 1.0 : -1.0) / (FMath::Abs(Theta) + FMath::Sqrt(Theta * Theta + 1));
					const double C = 1 / FMath::Sqrt(T * T + 1), S = T * C;
					for (int32 k = 0; k < 3; ++k)
					{
						const double Akp = A.M[k][p], Akq = A.M[k][q];
						A.M[k][p] = C * Akp - S * Akq;
						A.M[k][q] = S * Akp + C * Akq;
					}
					for (int32 k = 0; k < 3; ++k)
					{
						const double Apk = A.M[p][k], Aqk = A.M[q][k];
						A.M[p][k] = C * Apk - S * Aqk;
						A.M[q][k] = S * Apk + C * Aqk;
					}
					for (int32 k = 0; k < 3; ++k)
					{
						const double Vkp = V.M[k][p], Vkq = V.M[k][q];
						V.M[k][p] = C * Vkp - S * Vkq;
						V.M[k][q] = S * Vkp + C * Vkq;
					}
				}
			}
		}
		int32 Order[3] = { 0, 1, 2 };
		const double D[3] = { A.M[0][0], A.M[1][1], A.M[2][2] };
		// descending, stable (numpy's SVD order for ties is LAPACK's; keep the input axis order)
		for (int32 i = 0; i < 3; ++i) for (int32 j = i + 1; j < 3; ++j) if (D[Order[j]] > D[Order[i]] + 1e-12 * FMath::Max(1.0, FMath::Abs(D[Order[i]]))) Swap(Order[i], Order[j]);
		FM3 VS;
		for (int32 c = 0; c < 3; ++c)
		{
			Eval[c] = D[Order[c]];
			for (int32 r = 0; r < 3; ++r) VS.M[r][c] = V.M[r][Order[c]];
		}
		V = VS;
	}

	/** Orthogonal polar factor of L (numpy: u @ vt of the SVD, last column of u flipped when det < 0). */
	FM3 PolarRotation(const FM3& L)
	{
		double Ev[3];
		FM3 V;
		SymEigen(L.Transposed() * L, Ev, V);
		FM3 U = FM3::Zero();
		for (int32 c = 0; c < 3; ++c)
		{
			const double Sigma = FMath::Sqrt(FMath::Max(Ev[c], 0.0));
			const double Vc[3] = { V.M[0][c], V.M[1][c], V.M[2][c] };
			double Uc[3];
			L.Mul(Vc, Uc);
			for (int32 r = 0; r < 3; ++r) U.M[r][c] = Sigma > 1e-300 ? Uc[r] / Sigma : 0.0;
		}
		// complete degenerate columns
		auto Cross = [](const double A[3], const double B[3], double O[3]) { O[0] = A[1] * B[2] - A[2] * B[1]; O[1] = A[2] * B[0] - A[0] * B[2]; O[2] = A[0] * B[1] - A[1] * B[0]; };
		if (FMath::Sqrt(Ev[2] > 0 ? Ev[2] : 0) <= 1e-300)
		{
			const double A[3] = { U.M[0][0], U.M[1][0], U.M[2][0] }, B[3] = { U.M[0][1], U.M[1][1], U.M[2][1] };
			double C[3];
			Cross(A, B, C);
			for (int32 r = 0; r < 3; ++r) U.M[r][2] = C[r];
		}
		FM3 R = U * V.Transposed();
		if (R.Det() < 0)
		{
			for (int32 r = 0; r < 3; ++r) U.M[r][2] = -U.M[r][2];
			R = U * V.Transposed();
		}
		return R;
	}

	struct FXf
	{
		FM3 L;
		double T[3];
		bool bMirror = false;
		double S = 1.0;
		FM3 R;
		FQ QR;
		bool bUniform = true;

		FXf(const float Scale[3], const float Origin[3], const int32 Rot[3], double Global)
		{
			FM3 Sd = FM3::Zero();
			for (int32 i = 0; i < 3; ++i) Sd.M[i][i] = Scale[i];
			const FM3 Rot3 = RotatorMatrix(Rot[0], Rot[1], Rot[2]);
			L = Rot3 * Sd;
			for (auto& Row : L.M) for (double& V : Row) V *= Global;
			const double O[3] = { Origin[0], Origin[1], Origin[2] };
			double LO[3];
			L.Mul(O, LO);
			for (int32 i = 0; i < 3; ++i) T[i] = -LO[i];
			const double D = L.Det();
			bMirror = D < 0;
			S = D != 0 ? FMath::Pow(FMath::Abs(D), 1.0 / 3.0) : Global;
			R = PolarRotation(L);
			if (!MatToQuat(R, QR))
			{
				QR = FQ();
			}
			const double A0 = FMath::Abs(double(Scale[0]));
			bUniform = true;
			for (int32 i = 0; i < 3; ++i)
			{
				bUniform &= FMath::Abs(FMath::Abs(double(Scale[i])) - A0) <= 1e-8 + 1e-4 * A0;
			}
		}
		void Point(const double P[3], double Out[3]) const
		{
			L.Mul(P, Out);
			for (int32 i = 0; i < 3; ++i) Out[i] += T[i];
		}
		void Normal(const double N[3], double Out[3]) const
		{
			// row vector n @ inv(L)
			const FM3 I = L.Inverse();
			for (int32 c = 0; c < 3; ++c) Out[c] = N[0] * I.M[0][c] + N[1] * I.M[1][c] + N[2] * I.M[2][c];
			double Ln = FMath::Sqrt(Out[0] * Out[0] + Out[1] * Out[1] + Out[2] * Out[2]);
			if (Ln == 0) Ln = 1;
			for (int32 c = 0; c < 3; ++c) Out[c] /= Ln;
		}
	};

	// ---- parsed structures ----
	struct FWedge { uint16 V; float U, T; };
	struct FFace { uint16 W[3]; uint16 Mat; };
	struct FTri { uint16 W[3]; uint8 Mat; };
	struct FInf { float W; uint16 P, B; };
	struct FSection { uint16 Mat, MinStream, MinWedge, MaxWedge, NumStream, Bone, Fe, FirstFace, NumFaces; };
	struct FStreamVert { float P[3], N[3], UV[2]; };
	struct FBone { FString Name; uint32 Flags; float Quat[4]; float Pos[3]; int32 Parent; };
	struct FNotify { float Time; FString Function; int32 Object = 0; bool bHasObject = false; };
	struct FSeq { FString Name; TArray<FString> Groups; int32 StartFrame = 0, NumFrames = 0; TArray<FNotify> Notifies; float Rate = 0; };

	struct FLod
	{
		TArray<FSection> SoftSections, RigidSections;
		TArray<uint16> RigidIndices;
		/** Packed soft-skinned render stream (see DecodeSkinningData) and its triangle list (soft wedge indices). */
		TArray<uint32> SkinningData;
		TArray<uint16> SoftIndices;
		int32 NumSoftWedges = 0;
		TArray<FStreamVert> StreamVerts;
		TArray<FInf> Influences;
		TArray<FWedge> Wedges;
		TArray<FFace> Faces;
		TArray<float> Points;
		float DistanceFactor = 0;
	};

	struct FMesh
	{
		int32 Version = 0;
		TArray<uint32> Verts, Verts2;
		TArray<int32> Textures;
		float MeshScale[3], MeshOrigin[3];
		int32 RotOrigin[3];
		TArray<FFace> Faces;
		TArray<FWedge> Wedges;
		TArray<TPair<uint32, int32>> Materials;
		// skeletal
		TArray<FBone> Bones;
		int32 Animation = 0;
		TArray<FString> AttachAliases, AttachBones;
		TArray<TArray<double>> AttachCoords;
		TArray<FLod> Lods;
		bool bHasBase = false;
		TArray<float> Points;
		TArray<FWedge> WedgesBase;
		TArray<FTri> Triangles;
		TArray<FInf> Influences;
		int32 KarmaProps = 0, CollisionMesh = 0;
		FConvJson BoneSpheres = FConvJson::Array(), BoneBoxes = FConvJson::Array();
		TArray<TPair<FString, TArray<double>>> SphereData; // bone, offset+radius
		// vert mesh
		TArray<FSeq> Sequences;
		int32 VertCount = 0, FrameCount = 0;
	};

	struct FTrack { TArray<float> Quat, Pos, Time; };
	struct FMove { float RootSpeed[3]; float TrackTime; TArray<FTrack> Tracks; };
	struct FAnim
	{
		TArray<TPair<FString, int32>> Bones;
		TArray<FMove> Moves;
		TArray<FSeq> Sequences;
	};

	template <typename T>
	bool ReadArr(FConvReader& R, int32 ElemSize, TArray<T>& Out, FString& Err)
	{
		const int32 N = R.Index();
		if (R.bError || N < 0 || int64(N) * ElemSize > R.Remaining())
		{
			Err = FString::Printf(TEXT("bad array count %d (elem %d, %lld left)"), N, ElemSize, R.Remaining());
			return false;
		}
		Out.SetNumUninitialized(N);
		if (N)
		{
			static_assert(sizeof(T) <= 8, "plain element types only");
			check(sizeof(T) == ElemSize);
			R.Raw(Out.GetData(), int64(N) * ElemSize);
		}
		return true;
	}

	bool SkipArr(FConvReader& R, int32 ElemSize, FString& Err)
	{
		const int32 N = R.Index();
		if (R.bError || N < 0 || int64(N) * ElemSize > R.Remaining())
		{
			Err = FString::Printf(TEXT("bad array count %d (elem %d, %lld left)"), N, ElemSize, R.Remaining());
			return false;
		}
		R.Skip(int64(N) * ElemSize);
		return true;
	}

	// packed element readers (structs with padding cannot be memcpy'd)
	bool ReadWedges(FConvReader& R, TArray<FWedge>& Out, FString& Err)
	{
		const int32 N = R.Index();
		if (R.bError || N < 0 || int64(N) * 10 > R.Remaining()) { Err = TEXT("bad wedge count"); return false; }
		Out.SetNum(N);
		for (FWedge& W : Out) { W.V = R.U16(); W.U = R.F32(); W.T = R.F32(); }
		return !R.bError;
	}
	bool ReadFaces(FConvReader& R, TArray<FFace>& Out, FString& Err)
	{
		const int32 N = R.Index();
		if (R.bError || N < 0 || int64(N) * 8 > R.Remaining()) { Err = TEXT("bad face count"); return false; }
		Out.SetNum(N);
		for (FFace& F : Out) { F.W[0] = R.U16(); F.W[1] = R.U16(); F.W[2] = R.U16(); F.Mat = R.U16(); }
		return !R.bError;
	}
	bool ReadTris(FConvReader& R, TArray<FTri>& Out, FString& Err)
	{
		const int32 N = R.Index();
		if (R.bError || N < 0 || int64(N) * 12 > R.Remaining()) { Err = TEXT("bad triangle count"); return false; }
		Out.SetNum(N);
		for (FTri& T : Out) { T.W[0] = R.U16(); T.W[1] = R.U16(); T.W[2] = R.U16(); T.Mat = R.U8(); R.U8(); R.U32(); }
		return !R.bError;
	}
	bool ReadInfs(FConvReader& R, TArray<FInf>& Out, FString& Err)
	{
		const int32 N = R.Index();
		if (R.bError || N < 0 || int64(N) * 8 > R.Remaining()) { Err = TEXT("bad influence count"); return false; }
		Out.SetNum(N);
		for (FInf& I : Out) { I.W = R.F32(); I.P = R.U16(); I.B = R.U16(); }
		return !R.bError;
	}
	bool ReadSections(FConvReader& R, TArray<FSection>& Out, FString& Err)
	{
		const int32 N = R.Index();
		if (R.bError || N < 0 || int64(N) * 18 > R.Remaining()) { Err = TEXT("bad section count"); return false; }
		Out.SetNum(N);
		for (FSection& S : Out)
		{
			S.Mat = R.U16(); S.MinStream = R.U16(); S.MinWedge = R.U16(); S.MaxWedge = R.U16(); S.NumStream = R.U16();
			S.Bone = R.U16(); S.Fe = R.U16(); S.FirstFace = R.U16(); S.NumFaces = R.U16();
		}
		return !R.bError;
	}
	bool ReadStreamVerts(FConvReader& R, TArray<FStreamVert>& Out, FString& Err)
	{
		const int32 N = R.Index();
		if (R.bError || N < 0 || int64(N) * 32 > R.Remaining()) { Err = TEXT("bad stream vertex count"); return false; }
		Out.SetNumUninitialized(N);
		if (N) R.Raw(Out.GetData(), int64(N) * 32);
		return !R.bError;
	}
	bool ReadNames(FConvReader& R, FConvPackage* Pkg, TArray<FString>& Out, FString& Err)
	{
		const int32 N = R.Index();
		if (R.bError || N < 0 || N > R.Remaining()) { Err = TEXT("bad name array"); return false; }
		for (int32 i = 0; i < N; ++i)
		{
			FString S;
			if (!Pkg->NameAt(R.Index(), S)) { Err = TEXT("name index out of range"); return false; }
			Out.Add(S);
		}
		return !R.bError;
	}
	void Lazy(FConvReader& R, bool bSkip) { if (bSkip) R.I32(); }

	bool ReadLodMesh(FConvReader& R, FMesh& D, FString& Err)
	{
		R.Skip(25 + 16);
		D.Version = R.I32();
		R.I32();
		if (!ReadArr(R, 4, D.Verts, Err)) return false;
		if (D.Version <= 1 && !SkipArr(R, 38, Err)) return false;
		const int32 N = R.Index();
		if (R.bError || N < 0 || N > R.Remaining()) { Err = TEXT("bad texture count"); return false; }
		for (int32 i = 0; i < N; ++i) D.Textures.Add(R.Index());
		R.Vector(D.MeshScale);
		R.Vector(D.MeshOrigin);
		D.RotOrigin[0] = R.I32(); D.RotOrigin[1] = R.I32(); D.RotOrigin[2] = R.I32();
		if (D.Version <= 1 && !SkipArr(R, 2, Err)) return false;
		if (!SkipArr(R, 2, Err)) return false;
		if (!ReadFaces(R, D.Faces, Err)) return false;
		if (!SkipArr(R, 2, Err)) return false;
		if (!ReadWedges(R, D.Wedges, Err)) return false;
		{
			const int32 Nm = R.Index();
			if (R.bError || Nm < 0 || int64(Nm) * 8 > R.Remaining()) { Err = TEXT("bad material count"); return false; }
			for (int32 i = 0; i < Nm; ++i) { const uint32 F = R.U32(); const int32 T = R.I32(); D.Materials.Emplace(F, T); }
		}
		R.F32(); R.F32(); R.F32(); R.I32(); R.F32(); R.F32();
		if (D.Version >= 3)
		{
			R.I32(); R.Index(); R.Skip(12); R.Skip(12); R.Skip(12); R.U32(); R.I32(); R.I32(); R.I32();
		}
		if (D.Version >= 4) R.F32();
		if (R.bError) { Err = R.Error; return false; }
		return true;
	}

	bool ReadPointArray(FConvReader& R, TArray<float>& Out, FString& Err);

	bool ReadStaticLodFixed(FConvReader& R, bool bSkip, FLod& L, FString& Err)
	{
		if (!ReadArr(R, 4, L.SkinningData, Err) || !SkipArr(R, 16, Err)) return false;
		L.NumSoftWedges = R.I32();
		if (!ReadSections(R, L.SoftSections, Err) || !ReadSections(R, L.RigidSections, Err)) return false;
		if (!ReadArr(R, 2, L.SoftIndices, Err)) return false;
		R.I32();
		if (!ReadArr(R, 2, L.RigidIndices, Err)) return false;
		R.I32();
		R.I32(); R.I32(); R.I32();
		if (!ReadStreamVerts(R, L.StreamVerts, Err)) return false;
		Lazy(R, bSkip); if (!ReadInfs(R, L.Influences, Err)) return false;
		Lazy(R, bSkip); if (!ReadWedges(R, L.Wedges, Err)) return false;
		Lazy(R, bSkip); if (!ReadFaces(R, L.Faces, Err)) return false;
		Lazy(R, bSkip); if (!ReadPointArray(R, L.Points, Err)) return false;
		L.DistanceFactor = R.F32();
		R.F32(); R.I32(); R.I32(); R.I32(); R.I32();
		if (R.bError) { Err = R.Error; return false; }
		return true;
	}

	bool ReadPointArray(FConvReader& R, TArray<float>& Out, FString& Err)
	{
		const int32 N = R.Index();
		if (R.bError || N < 0 || int64(N) * 12 > R.Remaining()) { Err = TEXT("bad point count"); return false; }
		Out.SetNumUninitialized(N * 3);
		R.Raw(Out.GetData(), int64(N) * 12);
		return !R.bError;
	}

	bool ReadSkeletalMesh(FConvObject* Obj, FMesh& D, FString& Err)
	{
		FConvPackage* Pkg = Obj->Pkg;
		const int32 Ver = Pkg->Version;
		const bool bSkip = Ver > 61;
		FConvReader R = Obj->NativeReader();
		if (R.bError) { Err = R.Error; return false; }
		if (!ReadLodMesh(R, D, Err)) return false;
		{
			TArray<float> Points2;
			if (!ReadPointArray(R, Points2, Err)) return false;
			D.Points = Points2; // version <= 1 meshes use these
		}
		const int32 Nb = R.Index();
		if (R.bError || Nb <= 0 || Nb > 4096) { Err = FString::Printf(TEXT("bad bone count %d"), Nb); return false; }
		for (int32 i = 0; i < Nb; ++i)
		{
			FBone B;
			if (!Pkg->NameAt(R.Index(), B.Name)) { Err = TEXT("bone name out of range"); return false; }
			B.Flags = R.U32();
			R.Raw(B.Quat, 16);
			R.Vector(B.Pos);
			R.F32();
			R.Skip(12);
			R.I32();
			B.Parent = R.I32();
			D.Bones.Add(B);
		}
		D.Animation = R.Index();
		R.I32();
		const int32 Nwi = R.Index();
		for (int32 i = 0; i < Nwi && !R.bError; ++i)
		{
			if (!SkipArr(R, 2, Err)) return false;
			R.I32();
		}
		if (!SkipArr(R, 4, Err)) return false;
		if (!ReadNames(R, Pkg, D.AttachAliases, Err) || !ReadNames(R, Pkg, D.AttachBones, Err)) return false;
		const int32 Nc = R.Index();
		if (R.bError || Nc < 0 || int64(Nc) * 48 > R.Remaining()) { Err = TEXT("bad attach coords"); return false; }
		for (int32 i = 0; i < Nc; ++i)
		{
			float C[12];
			R.Raw(C, 48);
			TArray<double> Cd;
			for (float F : C) Cd.Add(F);
			D.AttachCoords.Add(Cd);
		}
		if (D.Version <= 1)
		{
			if (!SkipArr(R, 18, Err) || !SkipArr(R, 18, Err) || !SkipArr(R, 2, Err)) return false;
			D.bHasBase = false;
		}
		else
		{
			const int32 Nl = R.Index();
			if (R.bError || Nl < 0 || Nl > 32) { Err = FString::Printf(TEXT("bad LOD count %d"), Nl); return false; }
			for (int32 i = 0; i < Nl; ++i)
			{
				FLod L;
				if (!ReadStaticLodFixed(R, bSkip, L, Err)) return false;
				D.Lods.Add(MoveTemp(L));
			}
			R.Index();
			Lazy(R, bSkip); if (!ReadPointArray(R, D.Points, Err)) return false;
			Lazy(R, bSkip); if (!ReadWedges(R, D.WedgesBase, Err)) return false;
			Lazy(R, bSkip); if (!ReadTris(R, D.Triangles, Err)) return false;
			Lazy(R, bSkip); if (!ReadInfs(R, D.Influences, Err)) return false;
			Lazy(R, bSkip); if (!SkipArr(R, 2, Err)) return false;
			Lazy(R, bSkip); if (!SkipArr(R, 2, Err)) return false;
			D.bHasBase = true;
		}
		if (Ver >= 120) R.U32();
		if (R.bError) { Err = R.Error; return false; }
		if (Ver >= 122)
		{
			FConvReader Save = R;
			FConvJson Spheres = FConvJson::Array(), Boxes = FConvJson::Array();
			auto Fill = [&]()
			{
				D.KarmaProps = R.Index();
				const int32 Ns = R.Index();
				for (int32 i = 0; i < Ns && !R.bError; ++i)
				{
					FString Bone;
					if (!Pkg->NameAt(R.Index(), Bone)) { R.Fail(TEXT("name")); return; }
					float Off[3]; R.Vector(Off);
					const float Rad = R.F32();
					FConvJson S = FConvJson::Object();
					S.Set(TEXT("bone"), Bone);
					FConvJson O = FConvJson::Array(); O.Add(double(Off[0])); O.Add(double(Off[1])); O.Add(double(Off[2]));
					S.Set(TEXT("offset"), O);
					S.Set(TEXT("radius"), double(Rad));
					if (Ver > 123) S.Set(TEXT("block_karma"), R.I32());
					if (Ver > 124) { S.Set(TEXT("block_zero_extent"), R.I32()); S.Set(TEXT("block_nonzero_extent"), R.I32()); }
					if (!R.bError) Spheres.Add(S);
				}
				const int32 Nbx = R.Index();
				for (int32 i = 0; i < Nbx && !R.bError; ++i)
				{
					FString Bone;
					if (!Pkg->NameAt(R.Index(), Bone)) { R.Fail(TEXT("name")); return; }
					float Off[3]; R.Vector(Off);
					float Rad[3]; R.Vector(Rad);
					FConvJson S = FConvJson::Object();
					S.Set(TEXT("bone"), Bone);
					FConvJson O = FConvJson::Array(); O.Add(double(Off[0])); O.Add(double(Off[1])); O.Add(double(Off[2]));
					S.Set(TEXT("offset"), O);
					FConvJson Ra = FConvJson::Array(); Ra.Add(double(Rad[0])); Ra.Add(double(Rad[1])); Ra.Add(double(Rad[2]));
					S.Set(TEXT("radii"), Ra);
					if (Ver > 123) S.Set(TEXT("block_karma"), R.I32());
					if (Ver > 124) { S.Set(TEXT("block_zero_extent"), R.I32()); S.Set(TEXT("block_nonzero_extent"), R.I32()); }
					if (!R.bError) Boxes.Add(S);
				}
				const int32 Nx = R.Index();
				for (int32 i = 0; i < Nx && !R.bError; ++i) R.Index();
				if (Ver >= 127) D.CollisionMesh = R.Index();
			};
			Fill();
			D.BoneSpheres = Spheres;
			D.BoneBoxes = Boxes;
			if (R.bError && D.CollisionMesh == 0)
			{
				// Python keeps what was read before the error
			}
			(void)Save;
		}
		return true;
	}

	bool ReadSeqs(FConvReader& R, FConvPackage* Pkg, int32 Count, TArray<FSeq>& Out, FString& Err)
	{
		const int32 Ver = Pkg->Version;
		for (int32 i = 0; i < Count && !R.bError; ++i)
		{
			FSeq S;
			if (Ver >= 115) R.F32();
			if (!Pkg->NameAt(R.Index(), S.Name)) { Err = TEXT("sequence name out of range"); return false; }
			if (!ReadNames(R, Pkg, S.Groups, Err)) return false;
			S.StartFrame = R.I32();
			S.NumFrames = R.I32();
			const int32 Nn = R.Index();
			for (int32 k = 0; k < Nn && !R.bError; ++k)
			{
				FNotify N;
				N.Time = R.F32();
				if (!Pkg->NameAt(R.Index(), N.Function)) { Err = TEXT("notify name out of range"); return false; }
				if (Ver >= 112) { N.Object = R.Index(); N.bHasObject = true; }
				S.Notifies.Add(N);
			}
			S.Rate = R.F32();
			Out.Add(MoveTemp(S));
		}
		if (R.bError) { Err = R.Error; return false; }
		return true;
	}

	bool ReadAnimation(FConvObject* Obj, FAnim& A, FString& Err)
	{
		FConvPackage* Pkg = Obj->Pkg;
		FConvReader R = Obj->NativeReader();
		if (R.bError) { Err = R.Error; return false; }
		R.I32();
		const int32 Nb = R.Index();
		if (R.bError || Nb < 0 || Nb > 4096) { Err = FString::Printf(TEXT("bad ref bone count %d"), Nb); return false; }
		for (int32 i = 0; i < Nb; ++i)
		{
			FString N;
			if (!Pkg->NameAt(R.Index(), N)) { Err = TEXT("bone name out of range"); return false; }
			R.U32();
			const int32 Parent = R.I32();
			A.Bones.Emplace(N, Parent);
		}
		const int32 Nm = R.Index();
		if (R.bError || Nm < 0 || Nm > 100000) { Err = FString::Printf(TEXT("bad move count %d"), Nm); return false; }
		auto Track = [&](FTrack& T) -> bool
		{
			R.U32();
			if (!ReadArr(R, 4, T.Quat, Err)) return false; // placeholder, replaced below
			return true;
		};
		(void)Track;
		auto ReadFloats = [&](int32 Elem, TArray<float>& Out) -> bool
		{
			const int32 N = R.Index();
			if (R.bError || N < 0 || int64(N) * Elem > R.Remaining()) { Err = FString::Printf(TEXT("bad array count %d"), N); return false; }
			Out.SetNumUninitialized(N * (Elem / 4));
			if (N) R.Raw(Out.GetData(), int64(N) * Elem);
			return !R.bError;
		};
		auto ReadTrack = [&](FTrack& T) -> bool
		{
			R.U32();
			return ReadFloats(16, T.Quat) && ReadFloats(12, T.Pos) && ReadFloats(4, T.Time);
		};
		for (int32 i = 0; i < Nm; ++i)
		{
			FMove M;
			R.Vector(M.RootSpeed);
			M.TrackTime = R.F32();
			R.I32();
			const uint32 Flags = R.U32();
			TArray<float> Dummy;
			if (!ReadFloats(4, Dummy)) return false;
			const int32 Nt = R.Index();
			if (R.bError || Nt < 0 || Nt > 4096) { Err = FString::Printf(TEXT("bad track count %d"), Nt); return false; }
			M.Tracks.SetNum(Nt);
			for (FTrack& T : M.Tracks)
			{
				if (!ReadTrack(T)) return false;
			}
			FTrack Root;
			if (!ReadTrack(Root)) return false;
			if (Flags >= 3) { Err = TEXT("flex (compressed) animation tracks are not used by UT2004"); return false; }
			A.Moves.Add(MoveTemp(M));
		}
		const int32 Ns = R.Index();
		if (R.bError || Ns < 0 || Ns > 100000) { Err = FString::Printf(TEXT("bad sequence count %d"), Ns); return false; }
		return ReadSeqs(R, Pkg, Ns, A.Sequences, Err);
	}

	bool ReadVertMesh(FConvObject* Obj, FMesh& D, FString& Err)
	{
		FConvReader R = Obj->NativeReader();
		if (R.bError) { Err = R.Error; return false; }
		if (!ReadLodMesh(R, D, Err)) return false;
		if (!SkipArr(R, 32, Err)) return false;
		R.I32();
		if (!ReadArr(R, 4, D.Verts2, Err)) return false;
		if (!SkipArr(R, 4, Err)) return false;
		const int32 Ns = R.Index();
		if (!ReadSeqs(R, Obj->Pkg, Ns, D.Sequences, Err)) return false;
		if (!SkipArr(R, 4, Err)) return false;
		D.VertCount = R.I32();
		D.FrameCount = R.I32();
		if (R.bError) { Err = R.Error; return false; }
		return true;
	}

	// ---- geometry (skeletal.py mesh_geometry) ----
	struct FGeo
	{
		TArray<double> Positions; // 3 per wedge
		TArray<double> Normals;   // optional
		TArray<float> UVs;        // 2 per wedge
		TArray<int32> Joints;     // 4 per wedge
		TArray<double> Weights;   // 4 per wedge
		TArray<int32> Tris;       // 3 per tri
		TArray<int32> Mat;        // per tri
		int32 Unweighted = 0;
		int32 PointCount = 0;
		FString Source;
	};

	bool GeometryFrom(const TArray<float>& Points, const TArray<FWedge>& Wedges, const TArray<int32>& FacesW, const TArray<int32>& FacesMat,
		const TArray<FInf>& Infs, int32 NBones, FGeo& G, FString& Err)
	{
		const int32 NPts = Points.Num() / 3;
		if (NPts == 0 || Wedges.Num() == 0 || FacesW.Num() == 0) { Err = TEXT("empty geometry"); return false; }
		for (const FWedge& W : Wedges) if (W.V >= NPts) { Err = TEXT("wedge point index out of range"); return false; }
		for (int32 F : FacesW) if (F >= Wedges.Num()) { Err = TEXT("face wedge index out of range"); return false; }
		TArray<int32> Joints; Joints.SetNumZeroed(NPts * 4);
		TArray<double> Weights; Weights.SetNumZeroed(NPts * 4);
		TArray<int32> Count; Count.SetNumZeroed(NPts);
		if (Infs.Num())
		{
			TArray<int32> Order;
			for (int32 i = 0; i < Infs.Num(); ++i) Order.Add(i);
			// argsort(-w, stable): descending by weight, ties keep index order (NaN weights sort last in numpy)
			Algo::StableSort(Order, [&Infs](int32 A, int32 B)
			{
				const float Wa = Infs[A].W, Wb = Infs[B].W;
				if (FMath::IsNaN(Wa)) return false;
				if (FMath::IsNaN(Wb)) return true;
				return -Wa < -Wb;
			});
			for (int32 K : Order)
			{
				const FInf& I = Infs[K];
				const int32 P = I.P;
				int32 B = I.B;
				const double W = I.W;
				if (P >= NPts || !(W > 0.0)) continue;
				if (B >= NBones) B = 0;
				const int32 C = Count[P];
				if (C < 4)
				{
					int32 Hit = -1;
					for (int32 j = 0; j < C; ++j) if (Joints[P * 4 + j] == B) { Hit = j; break; }
					if (Hit >= 0) Weights[P * 4 + Hit] += W;
					else { Joints[P * 4 + C] = B; Weights[P * 4 + C] = W; Count[P] = C + 1; }
				}
			}
		}
		int32 Missing = 0;
		for (int32 p = 0; p < NPts; ++p)
		{
			if (Count[p] == 0) { Weights[p * 4] = 1.0; ++Missing; }
			double S = Weights[p * 4] + Weights[p * 4 + 1] + Weights[p * 4 + 2] + Weights[p * 4 + 3];
			if (S <= 0) S = 1.0;
			for (int32 j = 0; j < 4; ++j) Weights[p * 4 + j] /= S;
		}
		for (const FWedge& W : Wedges)
		{
			for (int32 c = 0; c < 3; ++c) G.Positions.Add(Points[W.V * 3 + c]);
			G.UVs.Add(W.U); G.UVs.Add(W.T);
			for (int32 j = 0; j < 4; ++j) { G.Joints.Add(Joints[W.V * 4 + j]); G.Weights.Add(Weights[W.V * 4 + j]); }
		}
		G.Tris = FacesW;
		G.Mat = FacesMat;
		G.Unweighted = Missing;
		G.PointCount = NPts;
		return true;
	}

	/**
	 * FStaticLODModel::SkinningData, the packed stream UE2 skins soft vertices from (layout found by inspection; it
	 * matches NumSoftWedges / points / NumSharedVerts of every LOD exactly). One entry per soft wedge, in wedge order:
	 *   new vertex:   N+1 influence DWORDs (first one: bit 31 = "shared later", bits 28-30 = N extra influences;
	 *                 each: bits 12-27 weight / 65535, bits 0-11 bone index * 6), then U, V floats. Consumes the
	 *                 next point of the LOD's Points array.
	 *   shared:       0xF0000000 | (k * 6), then U, V: the same point as the k-th "shared later" vertex.
	 *   end:          0xFFFFFFFF
	 * Triangles: SoftIndices (wedge indices) of each soft section.
	 */
	bool DecodeSkinningData(const FLod& L, int32 NBones, FGeo& G, FString& Err)
	{
		const TArray<uint32>& W = L.SkinningData;
		const int32 NPts = L.Points.Num() / 3;
		TArray<int32> Shared;
		int32 NextPoint = 0;
		int32 i = 0;
		auto F = [](uint32 X) { float V; FMemory::Memcpy(&V, &X, 4); return V; };
		while (i < W.Num() && W[i] != 0xFFFFFFFFu)
		{
			const uint32 X = W[i];
			const uint32 Top = X >> 28;
			int32 Point;
			if (Top == 0xF)
			{
				const uint32 Ref = X & 0x0FFFFFFF;
				if (Ref % 6 || int32(Ref / 6) >= Shared.Num() || i + 2 >= W.Num())
				{
					Err = FString::Printf(TEXT("bad shared vertex reference %u at %d"), Ref, i);
					return false;
				}
				Point = Shared[Ref / 6];
				G.UVs.Add(F(W[i + 1]));
				G.UVs.Add(F(W[i + 2]));
				i += 3;
			}
			else
			{
				const int32 N = int32(Top & 7) + 1;
				if (i + N + 1 >= W.Num() || NextPoint >= NPts)
				{
					Err = FString::Printf(TEXT("stream ends inside a vertex (word %d, point %d of %d)"), i, NextPoint, NPts);
					return false;
				}
				Point = NextPoint++;
				if (Top & 8)
				{
					Shared.Add(Point);
				}
				G.UVs.Add(F(W[i + N]));
				G.UVs.Add(F(W[i + N + 1]));
				i += N + 2;
			}
			for (int32 c = 0; c < 3; ++c)
			{
				G.Positions.Add(L.Points[Point * 3 + c]);
			}
			G.Mat.Add(Point); // temporarily: point of each wedge (replaced below)
		}
		if (i >= W.Num() || NextPoint != NPts)
		{
			Err = FString::Printf(TEXT("stream does not match the LOD (%d of %d points, %s)"), NextPoint, NPts, i >= W.Num() ? TEXT("no end marker") : TEXT("ended"));
			return false;
		}
		// influences per point (second pass: shared wedges reuse their point's influences)
		TArray<int32> PointJoints, PointWeightsCount;
		TArray<double> PointWeights;
		PointJoints.SetNumZeroed(NPts * 4);
		PointWeights.SetNumZeroed(NPts * 4);
		{
			int32 j = 0, P = 0;
			while (j < W.Num() && W[j] != 0xFFFFFFFFu)
			{
				const uint32 Top = W[j] >> 28;
				if (Top == 0xF)
				{
					j += 3;
					continue;
				}
				const int32 N = int32(Top & 7) + 1;
				// strongest 4, like the base-mesh path
				TArray<TPair<double, int32>> Infl;
				for (int32 k = 0; k < N; ++k)
				{
					const uint32 Iw = W[j + k];
					int32 Bone = int32(Iw & 0xFFF) / 6;
					if (Bone >= NBones) Bone = 0;
					Infl.Emplace(double((Iw >> 12) & 0xFFFF) / 65535.0, Bone);
				}
				Algo::StableSort(Infl, [](const TPair<double, int32>& A, const TPair<double, int32>& B) { return A.Key > B.Key; });
				double Sum = 0;
				for (int32 k = 0; k < FMath::Min(4, Infl.Num()); ++k)
				{
					PointJoints[P * 4 + k] = Infl[k].Value;
					PointWeights[P * 4 + k] = Infl[k].Key;
					Sum += Infl[k].Key;
				}
				if (Sum <= 0)
				{
					PointWeights[P * 4] = 1.0;
					Sum = 1.0;
				}
				for (int32 k = 0; k < 4; ++k) PointWeights[P * 4 + k] /= Sum;
				++P;
				j += N + 2;
			}
		}
		const TArray<int32> WedgePoint = MoveTemp(G.Mat);
		G.Mat.Reset();
		for (int32 Wp : WedgePoint)
		{
			for (int32 k = 0; k < 4; ++k)
			{
				G.Joints.Add(PointJoints[Wp * 4 + k]);
				G.Weights.Add(PointWeights[Wp * 4 + k]);
			}
		}
		const int32 NW = WedgePoint.Num();
		if (NW != L.NumSoftWedges)
		{
			Err = FString::Printf(TEXT("%d wedges decoded, the LOD has %d"), NW, L.NumSoftWedges);
			return false;
		}
		for (const FSection& S : L.SoftSections)
		{
			const int32 A = S.FirstFace * 3;
			const int32 End = FMath::Min(A + S.NumFaces * 3, L.SoftIndices.Num());
			for (int32 k = A; k + 2 < End; k += 3)
			{
				for (int32 c = 0; c < 3; ++c)
				{
					const int32 Wi = L.SoftIndices[k + c];
					if (Wi >= NW)
					{
						Err = TEXT("soft index out of range");
						return false;
					}
					G.Tris.Add(Wi);
				}
				G.Mat.Add(S.Mat);
			}
		}
		if (!G.Tris.Num())
		{
			Err = TEXT("no triangles");
			return false;
		}
		G.PointCount = NPts;
		G.Unweighted = 0;
		return true;
	}

	bool MeshGeometry(const FMesh& M, FGeo& G, FString& Err)
	{
		const int32 Nb = M.Bones.Num();
		if (M.bHasBase && M.Points.Num() && M.WedgesBase.Num() && M.Triangles.Num() && M.Influences.Num())
		{
			TArray<int32> Fw, Fm;
			for (const FTri& T : M.Triangles) { Fw.Add(T.W[0]); Fw.Add(T.W[1]); Fw.Add(T.W[2]); Fm.Add(T.Mat); }
			if (!GeometryFrom(M.Points, M.WedgesBase, Fw, Fm, M.Influences, Nb, G, Err)) return false;
			G.Source = TEXT("base");
			return true;
		}
		for (int32 Li = 0; Li < M.Lods.Num(); ++Li)
		{
			const FLod& L = M.Lods[Li];
			if (!(L.Points.Num() && L.Wedges.Num() && L.Influences.Num())) continue;
			TArray<int32> Fw, Fm;
			for (const FSection& S : L.SoftSections)
			{
				for (int32 f = S.FirstFace; f < S.FirstFace + S.NumFaces && f < L.Faces.Num(); ++f)
				{
					Fw.Append({ (int32)L.Faces[f].W[0], (int32)L.Faces[f].W[1], (int32)L.Faces[f].W[2] });
					Fm.Add(S.Mat);
				}
			}
			for (const FSection& S : L.RigidSections)
			{
				const int32 A = S.FirstFace * 3;
				const int32 End = FMath::Min(A + S.NumFaces * 3, L.RigidIndices.Num());
				const int32 N = FMath::Max(0, End - A) / 3;
				for (int32 k = 0; k < N * 3; ++k) Fw.Add(L.RigidIndices[A + k]);
				for (int32 k = 0; k < N; ++k) Fm.Add(S.Mat);
			}
			if (L.SoftSections.Num() == 0 && L.RigidSections.Num() == 0)
			{
				for (const FFace& F : L.Faces) { Fw.Append({ (int32)F.W[0], (int32)F.W[1], (int32)F.W[2] }); Fm.Add(F.Mat); }
			}
			if (!GeometryFrom(L.Points, L.Wedges, Fw, Fm, L.Influences, Nb, G, Err)) return false;
			G.Source = FString::Printf(TEXT("lod%d"), Li);
			return true;
		}
		for (int32 Li = 0; Li < M.Lods.Num(); ++Li)
		{
			const FLod& L = M.Lods[Li];
			if (L.SoftSections.Num() || !L.RigidSections.Num() || !L.StreamVerts.Num()) continue;
			const int32 Nsv = L.StreamVerts.Num();
			TArray<int32> Joints; Joints.SetNumZeroed(Nsv * 4);
			TArray<int32> Tris, Mats;
			for (const FSection& S : L.RigidSections)
			{
				const int32 A = S.FirstFace * 3;
				const int32 End = FMath::Min(A + S.NumFaces * 3, L.RigidIndices.Num());
				if (End <= A) continue;
				for (int32 k = A; k < End; ++k)
				{
					if (L.RigidIndices[k] >= Nsv) { Err = TEXT("rigid section index out of range"); return false; }
				}
				for (int32 k = A; k < End; ++k) Joints[L.RigidIndices[k] * 4] = FMath::Min<int32>(S.Bone, Nb - 1);
				const int32 N = (End - A) / 3;
				for (int32 k = 0; k < N * 3; ++k) Tris.Add(L.RigidIndices[A + k]);
				for (int32 k = 0; k < (End - A) / 3; ++k) Mats.Add(S.Mat);
			}
			if (!Tris.Num()) continue;
			for (const FStreamVert& V : L.StreamVerts)
			{
				for (int32 c = 0; c < 3; ++c) { G.Positions.Add(V.P[c]); G.Normals.Add(V.N[c]); }
				G.UVs.Add(V.UV[0]); G.UVs.Add(V.UV[1]);
			}
			G.Joints = Joints;
			G.Weights.SetNumZeroed(Nsv * 4);
			for (int32 i = 0; i < Nsv; ++i) G.Weights[i * 4] = 1.0;
			G.Tris = Tris;
			G.Mat = Mats;
			G.Unweighted = 0;
			G.PointCount = Nsv;
			G.Source = FString::Printf(TEXT("lod%d-rigid-stream"), Li);
			return true;
		}
		// meshes shipped without their base mesh and unpacked LODs (AS-BP2-Outback.RooMesh): rebuild LOD geometry from
		// the packed soft-skinning stream the renderer uses
		FString DecodeFail;
		for (int32 Li = 0; Li < M.Lods.Num(); ++Li)
		{
			const FLod& L = M.Lods[Li];
			if (L.SoftSections.Num() && !L.RigidSections.Num() && L.SkinningData.Num() && L.Points.Num() && L.SoftIndices.Num())
			{
				FString DecodeErr;
				if (DecodeSkinningData(L, Nb, G, DecodeErr))
				{
					G.Source = FString::Printf(TEXT("lod%d-packed-soft"), Li);
					return true;
				}
				if (DecodeFail.IsEmpty())
				{
					DecodeFail = FString::Printf(TEXT("LOD %d: packed soft-skinning data could not be decoded: %s"), Li, *DecodeErr);
				}
				G = FGeo();
			}
		}
		if (!DecodeFail.IsEmpty())
		{
			Err = DecodeFail;
			return false;
		}
		for (const FLod& L : M.Lods)
		{
			if (L.SoftSections.Num())
			{
				Err = TEXT("only packed soft-skinned render data (SkinningData) is present; the base mesh was stripped from the package");
				return false;
			}
		}
		Err = TEXT("no usable geometry (base mesh and LODs empty)");
		return false;
	}

	// ---- skeleton ----
	TArray<FString> UniqueNames(const TArray<FString>& Names)
	{
		TMap<FString, int32> Seen;
		TArray<FString> Out;
		for (const FString& N : Names)
		{
			const FString Base = N.IsEmpty() ? FString(TEXT("Bone")) : N;
			const FString K = Base.ToLower();
			if (int32* Hit = Seen.Find(K))
			{
				++*Hit;
				FString NN = FString::Printf(TEXT("%s_%d"), *Base, *Hit);
				while (Seen.Contains(NN.ToLower()))
				{
					++*Seen.Find(K);
					NN = FString::Printf(TEXT("%s_%d"), *Base, Seen[K]);
				}
				Seen.Add(NN.ToLower(), 0);
				Out.Add(NN);
			}
			else
			{
				Seen.Add(K, 0);
				Out.Add(Base);
			}
		}
		return Out;
	}

	struct FSkel
	{
		TArray<FString> Names;
		TArray<int32> Parents;
		TArray<FQ> LocalRot;
		TArray<FVector3d> LocalPos;
		TArray<FM4> World;

		void ComputeWorld()
		{
			World.SetNum(Names.Num());
			for (int32 i = 0; i < Names.Num(); ++i)
			{
				FM4 M;
				M.R = QuatToMatrix(LocalRot[i]);
				M.T[0] = LocalPos[i].X; M.T[1] = LocalPos[i].Y; M.T[2] = LocalPos[i].Z;
				const int32 P = Parents[i];
				World[i] = (i == 0 || P < 0 || P >= i) ? M : World[P] * M;
			}
		}

		static FSkel FromMesh(const FMesh& D, const FXf& Xf)
		{
			FSkel Raw;
			TArray<FString> RawNames;
			for (int32 i = 0; i < D.Bones.Num(); ++i)
			{
				const FBone& B = D.Bones[i];
				FQ Q = { B.Quat[0], B.Quat[1], B.Quat[2], B.Quat[3] };
				Raw.LocalRot.Add(QNorm(i == 0 ? Q : QConj(Q)));
				Raw.LocalPos.Add(FVector3d(B.Pos[0], B.Pos[1], B.Pos[2]));
				Raw.Parents.Add(i ? B.Parent : -1);
				Raw.Names.Add(B.Name);
				RawNames.Add(B.Name);
			}
			Raw.ComputeWorld();
			TArray<FM4> Bw;
			for (const FM4& W : Raw.World)
			{
				FM4 N;
				N.R = Xf.R * W.R;
				Xf.Point(W.T, N.T);
				Bw.Add(N);
			}
			FSkel S;
			S.Names = UniqueNames(RawNames);
			S.Parents = Raw.Parents;
			for (int32 i = 0; i < Bw.Num(); ++i)
			{
				const int32 P = S.Parents[i];
				const FM4 Loc = P < 0 ? Bw[i] : Bw[P].Inverse() * Bw[i];
				FQ Q;
				if (!MatToQuat(Loc.R, Q)) Q = FQ();
				S.LocalRot.Add(Q);
				S.LocalPos.Add(FVector3d(Loc.T[0], Loc.T[1], Loc.T[2]));
			}
			S.ComputeWorld();
			return S;
		}
	};

	TArray<FQuat4d> ToUEQuats(const TArray<FQ>& Q)
	{
		TArray<FQuat4d> Out;
		for (const FQ& V : Q) Out.Add(FQuat4d(V.X, V.Y, V.Z, V.W));
		return Out;
	}

	/** skelexport.section_primitives */
	TArray<FConvPrim> SectionPrims(const FGeo& G, const FXf& Xf, const TArray<FString>& MatNames, bool bFlip, TArray<double>* OutAllPos = nullptr)
	{
		const int32 Nw = G.Positions.Num() / 3;
		TArray<double> Pos; Pos.SetNumUninitialized(Nw * 3);
		for (int32 i = 0; i < Nw; ++i) Xf.Point(&G.Positions[i * 3], &Pos[i * 3]);
		TArray<double> Nrm;
		if (G.Normals.Num())
		{
			Nrm.SetNumUninitialized(Nw * 3);
			for (int32 i = 0; i < Nw; ++i) Xf.Normal(&G.Normals[i * 3], &Nrm[i * 3]);
		}
		const bool bSwap = bFlip != Xf.bMirror;
		TArray<int32> MatsSorted = G.Mat;
		MatsSorted.Sort();
		MatsSorted.SetNum(Algo::Unique(MatsSorted));
		TArray<FConvPrim> Prims;
		const int32 NT = G.Mat.Num();
		for (int32 M : MatsSorted)
		{
			TArray<int32> T;
			for (int32 t = 0; t < NT; ++t)
			{
				if (G.Mat[t] != M) continue;
				T.Add(G.Tris[t * 3]);
				T.Add(bSwap ? G.Tris[t * 3 + 2] : G.Tris[t * 3 + 1]);
				T.Add(bSwap ? G.Tris[t * 3 + 1] : G.Tris[t * 3 + 2]);
			}
			TArray<int32> Used = T;
			Used.Sort();
			Used.SetNum(Algo::Unique(Used));
			TMap<int32, int32> Remap;
			for (int32 k = 0; k < Used.Num(); ++k) Remap.Add(Used[k], k);
			FConvPrim P;
			for (int32 U : Used)
			{
				for (int32 c = 0; c < 3; ++c)
				{
					P.Positions.Add((float)Pos[U * 3 + c]);
					if (OutAllPos) OutAllPos->Add(Pos[U * 3 + c]);
					if (Nrm.Num()) P.Normals.Add((float)Nrm[U * 3 + c]);
				}
				P.UVs.Add(G.UVs[U * 2]);
				P.UVs.Add(G.UVs[U * 2 + 1]);
				for (int32 j = 0; j < 4; ++j)
				{
					P.Joints.Add((uint16)G.Joints[U * 4 + j]);
					P.Weights.Add((float)G.Weights[U * 4 + j]);
				}
			}
			for (int32 I : T) P.Indices.Add(Remap[I]);
			P.Material = M >= 0 && M < MatNames.Num() ? MatNames[M] : FString(TEXT("None"));
			P.Extras = FConvJson::Object();
			P.Extras.Set(TEXT("section"), M);
			P.Extras.Set(TEXT("material_index"), M);
			Prims.Add(MoveTemp(P));
		}
		return Prims;
	}

	// ---- animation sampling ----
	FQ Slerp(const FQ& A, FQ B, double T)
	{
		double D = QDot(A, B);
		if (D < 0) { B = { -B.X, -B.Y, -B.Z, -B.W }; D = -D; }
		if (D > 0.9995)
		{
			FQ R = { A.X + T * (B.X - A.X), A.Y + T * (B.Y - A.Y), A.Z + T * (B.Z - A.Z), A.W + T * (B.W - A.W) };
			double N = FMath::Sqrt(QDot(R, R));
			if (N == 0) N = 1.0;
			return { R.X / N, R.Y / N, R.Z / N, R.W / N };
		}
		const double Th = FMath::Acos(FMath::Min(1.0, D));
		const double S = FMath::Sin(Th);
		const double Wa = FMath::Sin((1 - T) * Th) / S, Wb = FMath::Sin(T * Th) / S;
		return { Wa * A.X + Wb * B.X, Wa * A.Y + Wb * B.Y, Wa * A.Z + Wb * B.Z, Wa * A.W + Wb * B.W };
	}

	/** skeletal.sample_track: keys (Comp floats each) sampled at frames 0..NFrames-1. Empty when no keys. */
	TArray<double> SampleKeys(const TArray<float>& Keys, int32 Comp, const TArray<float>& Times, int32 NFrames, double TrackTime, bool bQuat)
	{
		TArray<double> Out;
		const int32 Nk = Keys.Num() / Comp;
		if (Nk == 0) return Out;
		Out.SetNumUninitialized(NFrames * Comp);
		TArray<double> T;
		for (float V : Times) T.Add(V);
		if (TrackTime > 0 && NFrames > 0) for (double& V : T) V = V * (double(NFrames) / TrackTime);
		if (Nk == 1 || T.Num() < 2)
		{
			for (int32 f = 0; f < NFrames; ++f) for (int32 c = 0; c < Comp; ++c) Out[f * Comp + c] = Keys[c];
			return Out;
		}
		TArray<double> Kt;
		if (T.Num() >= Nk) { for (int32 i = 0; i < Nk; ++i) Kt.Add(T[i]); }
		else
		{
			const double Stop = FMath::Max(NFrames - 1, 1);
			for (int32 i = 0; i < Nk; ++i) Kt.Add(Nk > 1 ? Stop * i / (Nk - 1) : 0.0);
		}
		for (int32 f = 0; f < NFrames; ++f)
		{
			const double F = f;
			int32 Lo = 0, Hi = Kt.Num();
			while (Lo < Hi) { const int32 Mid = (Lo + Hi) >> 1; if (Kt[Mid] <= F) Lo = Mid + 1; else Hi = Mid; }
			const int32 J = Lo - 1;
			double* O = &Out[f * Comp];
			if (J < 0) { for (int32 c = 0; c < Comp; ++c) O[c] = Keys[c]; continue; }
			if (J >= Nk - 1) { for (int32 c = 0; c < Comp; ++c) O[c] = Keys[(Nk - 1) * Comp + c]; continue; }
			const double T0 = Kt[J], T1 = Kt[J + 1];
			const double A = T1 <= T0 ? 0.0 : (F - T0) / (T1 - T0);
			if (bQuat)
			{
				const FQ Qa = { Keys[J * 4], Keys[J * 4 + 1], Keys[J * 4 + 2], Keys[J * 4 + 3] };
				const FQ Qb = { Keys[J * 4 + 4], Keys[J * 4 + 5], Keys[J * 4 + 6], Keys[J * 4 + 7] };
				const FQ R = Slerp(Qa, Qb, A);
				O[0] = R.X; O[1] = R.Y; O[2] = R.Z; O[3] = R.W;
			}
			else
			{
				for (int32 c = 0; c < Comp; ++c) O[c] = double(Keys[J * Comp + c]) * (1 - A) + double(Keys[(J + 1) * Comp + c]) * A;
			}
		}
		return Out;
	}

	struct FChan { TArray<FQ> Rot; TArray<FVector3d> Pos; };

	/** skelexport.sample_sequence */
	int32 SampleSequence(const FAnim& A, int32 SeqIndex, const FSkel& Sk, const FXf& Xf, TMap<int32, FChan>& Out, TArray<FString>& Unmatched)
	{
		const FSeq& Seq = A.Sequences[SeqIndex];
		const FMove* Move = SeqIndex < A.Moves.Num() ? &A.Moves[SeqIndex] : nullptr;
		const int32 Nf = FMath::Max(Seq.NumFrames, 1);
		TMap<FString, int32> ByName;
		for (int32 i = 0; i < Sk.Names.Num(); ++i) ByName.Add(Sk.Names[i].ToLower(), i);
		if (!Move)
		{
			for (const auto& B : A.Bones) Unmatched.Add(B.Key);
			return Nf;
		}
		for (int32 Ai = 0; Ai < A.Bones.Num(); ++Ai)
		{
			const int32* JiP = ByName.Find(A.Bones[Ai].Key.ToLower());
			if (!JiP) { Unmatched.Add(A.Bones[Ai].Key); continue; }
			const int32 Ji = *JiP;
			if (Ai >= Move->Tracks.Num()) continue;
			const FTrack& Tr = Move->Tracks[Ai];
			const TArray<double> Q = SampleKeys(Tr.Quat, 4, Tr.Time, Nf, Move->TrackTime, true);
			const TArray<double> P = SampleKeys(Tr.Pos, 3, Tr.Time, Nf, Move->TrackTime, false);
			const bool bRoot = Ai == 0 || Ji == 0;
			FChan C;
			TArray<FQ> QStd;
			if (Q.Num())
			{
				for (int32 f = 0; f < Nf; ++f)
				{
					FQ V = { Q[f * 4], Q[f * 4 + 1], Q[f * 4 + 2], Q[f * 4 + 3] };
					QStd.Add(QNorm(bRoot ? V : QConj(V)));
				}
			}
			if (QStd.Num()) C.Rot = QStd;
			else for (int32 f = 0; f < Nf; ++f) C.Rot.Add(Sk.LocalRot[Ji]);
			if (P.Num())
			{
				for (int32 f = 0; f < Nf; ++f)
				{
					if (bRoot)
					{
						double O[3];
						Xf.Point(&P[f * 3], O);
						C.Pos.Add(FVector3d(O[0], O[1], O[2]));
					}
					else
					{
						C.Pos.Add(FVector3d(P[f * 3], P[f * 3 + 1], P[f * 3 + 2]) * Xf.S);
					}
				}
			}
			else for (int32 f = 0; f < Nf; ++f) C.Pos.Add(Sk.LocalPos[Ji]);
			if (bRoot && QStd.Num())
			{
				for (FQ& R : C.Rot) R = QMul(Xf.QR, R);
			}
			for (int32 k = 1; k < C.Rot.Num(); ++k)
			{
				if (QDot(C.Rot[k], C.Rot[k - 1]) < 0) C.Rot[k] = { -C.Rot[k].X, -C.Rot[k].Y, -C.Rot[k].Z, -C.Rot[k].W };
			}
			Out.Add(Ji, MoveTemp(C));
		}
		return Nf;
	}

	FConvJson RotToDeg(const int32 R[3])
	{
		FConvJson O = FConvJson::Object();
		const TCHAR* K[3] = { TEXT("pitch"), TEXT("yaw"), TEXT("roll") };
		for (int32 i = 0; i < 3; ++i)
		{
			double D = double(((int64(R[i]) % 65536) + 65536) % 65536) * 360.0 / 65536.0;
			if (D > 180.0) D -= 360.0;
			O.Set(K[i], FConvJson::RoundTo(D, 5));
		}
		return O;
	}

	FConvJson F3(const float V[3]) { FConvJson A = FConvJson::Array(); A.Add(double(V[0])); A.Add(double(V[1])); A.Add(double(V[2])); return A; }
	FConvJson I3(const int32 V[3]) { FConvJson A = FConvJson::Array(); A.Add(V[0]); A.Add(V[1]); A.Add(V[2]); return A; }
	FConvJson BoundsOf(const TArray<double>& P)
	{
		double Mn[3] = { 0, 0, 0 }, Mx[3] = { 0, 0, 0 };
		for (int32 i = 0; i < P.Num() / 3; ++i)
		{
			for (int32 c = 0; c < 3; ++c)
			{
				if (i == 0) Mn[c] = Mx[c] = P[c];
				Mn[c] = FMath::Min(Mn[c], P[i * 3 + c]);
				Mx[c] = FMath::Max(Mx[c], P[i * 3 + c]);
			}
		}
		FConvJson B = FConvJson::Object();
		FConvJson A = FConvJson::Array(), C = FConvJson::Array();
		for (int32 c = 0; c < 3; ++c) { A.Add(FConvJson::RoundTo(Mn[c], 4)); C.Add(FConvJson::RoundTo(Mx[c], 4)); }
		B.Set(TEXT("min"), A);
		B.Set(TEXT("max"), C);
		return B;
	}

	FConvJson SectionsJson(const TArray<FConvPrim>& Prims)
	{
		FConvJson S = FConvJson::Array();
		for (const FConvPrim& P : Prims)
		{
			FConvJson E = FConvJson::Object();
			E.Set(TEXT("index"), P.Extras.Get(TEXT("section")));
			E.Set(TEXT("material"), P.Material);
			E.Set(TEXT("triangles"), P.Indices.Num() / 3);
			S.Add(E);
		}
		return S;
	}
}

using namespace ArenaConvSkel;

namespace ArenaConvSkelExport
{
	TArray<FString> MeshMaterialNames(FConvExporter& Ex, FConvObject* Obj, const FMesh& D)
	{
		TArray<FString> Names;
		for (const TPair<uint32, int32>& M : D.Materials)
		{
			const int32 Ti = M.Value;
			FConvRef Ref;
			if (Ti >= 0 && Ti < D.Textures.Num())
			{
				Ref = Obj->Pkg->MakeRef(D.Textures[Ti]);
			}
			if (Ref.IsSet())
			{
				Ex.ExportRef(Ref);
				Names.Add(Ex.Canon(Ref));
			}
			else
			{
				Names.Add(TEXT("None"));
			}
		}
		return Names;
	}

	FConvJson RefPathOrNull(FConvExporter& Ex, FConvPackage* Pkg, int32 Index)
	{
		if (!Index) return FConvJson();
		return FConvJson(Ex.Canon(Pkg->MakeRef(Index)));
	}
}

using namespace ArenaConvSkelExport;

const TMap<FString, TArray<FString>>& FConvExporter::AnimMeshIndex()
{
	if (AnimMeshes)
	{
		return *AnimMeshes;
	}
	AnimMeshes = new TMap<FString, TArray<FString>>();
	for (const TPair<FString, FString>& KV : Mgr.PackageFiles())
	{
		const FString Ext = FPaths::GetExtension(KV.Value, true).ToLower();
		if (Ext == TEXT(".ut2") || !(Ext == TEXT(".ukx") || Ext == TEXT(".u") || Ext == TEXT(".utx") || Ext == TEXT(".usx")))
		{
			continue;
		}
		FConvPackage* Pkg = Mgr.LoadPackage(KV.Key);
		if (!Pkg)
		{
			continue;
		}
		for (int32 I : Pkg->ExportsOfClass({ TEXT("SkeletalMesh") }))
		{
			FConvObject* O = Pkg->GetObject(I);
			FMesh D;
			FString Err;
			if (O && ReadSkeletalMesh(O, D, Err) && D.Animation)
			{
				const FString A = Canon(Pkg->MakeRef(D.Animation));
				AnimMeshes->FindOrAdd(A.ToLower()).Add(O->Path());
			}
		}
	}
	return *AnimMeshes;
}

FString FConvExporter::ExportSkeletalMesh(FConvObject* Obj)
{
	const FString GPath = OutPath(TEXT("skeletalmeshes"), Obj, TEXT(".gltf"));
	const FString What = FString::Printf(TEXT("%s (%s)"), *Obj->Path(), *Obj->ClassName());
	FMesh D;
	FString Err;
	if (!ReadSkeletalMesh(Obj, D, Err))
	{
		Error(What, Err);
		return FString();
	}
	const FConvRef AnimRef = D.Animation ? Obj->Pkg->MakeRef(D.Animation) : FConvRef();
	if (Fresh(Obj, GPath))
	{
		MeshMaterialNames(*this, Obj, D);
		if (AnimRef.IsSet())
		{
			ExportRef(AnimRef);
		}
		return Rel(GPath);
	}
	FGeo Geo;
	if (!MeshGeometry(D, Geo, Err))
	{
		Error(What, Err);
		return FString();
	}
	const FXf Xf(D.MeshScale, D.MeshOrigin, D.RotOrigin, Opt.Scale);
	const FSkel Sk = FSkel::FromMesh(D, Xf);
	const TArray<FString> MatNames = MeshMaterialNames(*this, Obj, D);
	TArray<double> AllPos;
	const TArray<FConvPrim> Prims = SectionPrims(Geo, Xf, MatNames, false, &AllPos);
	FConvGltf Gb(Opt.Scale);
	TArray<int32> Roots, Joints;
	Gb.AddSkeleton(Sk.Names, Sk.Parents, ToUEQuats(Sk.LocalRot), Sk.LocalPos, Roots, Joints);
	TArray<FMatrix44d> W;
	for (const FM4& M : Sk.World) W.Add(M.ToUE());
	const int32 Skin = Gb.AddSkin(W, Joints, Roots[0]);
	const int32 MNode = Gb.AddSkinnedMesh(Obj->Name(), Prims, Skin);
	TArray<int32> SceneNodes = Roots;
	SceneNodes.Add(MNode);
	FConvJson Extra = FConvJson::Object();
	Extra.Set(TEXT("source"), Obj->Path());
	Gb.WriteSkinned(GPath, SceneNodes, Extra);
	if (AllPos.Num() == 0)
	{
		AllPos = { 0, 0, 0 };
	}
	// sockets
	FConvJson Sockets = FConvJson::Array();
	for (int32 i = 0; i < D.AttachAliases.Num() && i < D.AttachBones.Num() && i < D.AttachCoords.Num(); ++i)
	{
		const FString& Bone = D.AttachBones[i];
		const TArray<double>& C = D.AttachCoords[i];
		int32 Bi = INDEX_NONE;
		for (int32 b = 0; b < D.Bones.Num(); ++b)
		{
			if (D.Bones[b].Name.ToLower() == Bone.ToLower()) { Bi = b; break; }
		}
		FM3 Axes;
		for (int32 r = 0; r < 3; ++r) for (int32 c = 0; c < 3; ++c) Axes.M[r][c] = C[(c + 1) * 3 + r];
		FQ Q;
		if (!MatToQuat(Axes, Q)) Q = FQ();
		FConvJson S = FConvJson::Object();
		S.Set(TEXT("name"), D.AttachAliases[i]);
		S.Set(TEXT("bone"), Bi != INDEX_NONE ? Sk.Names[Bi] : Bone);
		S.Set(TEXT("bone_original"), Bone);
		FConvJson Loc = FConvJson::Array();
		for (int32 c = 0; c < 3; ++c) Loc.Add(FConvJson::RoundTo(C[c] * Xf.S, 5));
		S.Set(TEXT("location"), Loc);
		FConvJson Rq = FConvJson::Array();
		Rq.Add(FConvJson::RoundTo(Q.X, 6)); Rq.Add(FConvJson::RoundTo(Q.Y, 6)); Rq.Add(FConvJson::RoundTo(Q.Z, 6)); Rq.Add(FConvJson::RoundTo(Q.W, 6));
		S.Set(TEXT("rotation_quat"), Rq);
		S.Set(TEXT("rotation"), QuatToRotator(Q));
		FConvJson RawC = FConvJson::Object();
		const TCHAR* Keys[4] = { TEXT("origin"), TEXT("x_axis"), TEXT("y_axis"), TEXT("z_axis") };
		for (int32 k = 0; k < 4; ++k)
		{
			FConvJson V = FConvJson::Array();
			for (int32 c = 0; c < 3; ++c) V.Add(C[k * 3 + c]);
			RawC.Set(Keys[k], V);
		}
		S.Set(TEXT("raw_coords"), RawC);
		Sockets.Add(S);
	}
	const FString AnimPath = AnimRef.IsSet() ? Canon(AnimRef) : FString();
	FConvJson Meta = FConvJson::Object();
	Meta.Set(TEXT("type"), TEXT("SkeletalMesh"));
	Meta.Set(TEXT("path"), Obj->Path());
	Meta.Set(TEXT("scale"), Opt.Scale);
	Meta.Set(TEXT("tool_version"), ToolVersion);
	Meta.Set(TEXT("gltf"), FPaths::GetCleanFilename(GPath));
	Meta.Set(TEXT("geometry_source"), Geo.Source);
	int32 NV = 0;
	for (const FConvPrim& P : Prims) NV += P.NumVerts();
	Meta.Set(TEXT("vertex_count"), NV);
	Meta.Set(TEXT("triangle_count"), Geo.Mat.Num());
	Meta.Set(TEXT("point_count"), Geo.PointCount);
	Meta.Set(TEXT("unweighted_points"), Geo.Unweighted);
	Meta.Set(TEXT("bounds"), BoundsOf(AllPos));
	FConvJson Mt = FConvJson::Object();
	Mt.Set(TEXT("mesh_scale"), F3(D.MeshScale));
	Mt.Set(TEXT("mesh_origin"), F3(D.MeshOrigin));
	Mt.Set(TEXT("rot_origin"), I3(D.RotOrigin));
	Mt.Set(TEXT("rot_origin_deg"), RotToDeg(D.RotOrigin));
	Mt.Set(TEXT("baked"), true);
	Mt.Set(TEXT("mirrored"), Xf.bMirror);
	Mt.Set(TEXT("uniform_scale"), Xf.bUniform);
	Mt.Set(TEXT("bone_scale"), FConvJson::RoundTo(Xf.S, 6));
	Mt.Set(TEXT("note"), TEXT("actor = R(RotOrigin) * (MeshScale * (v - MeshOrigin)) * scale, baked into vertices, bind pose and animations"));
	Meta.Set(TEXT("mesh_transform"), Mt);
	FConvJson Bones = FConvJson::Array();
	for (int32 i = 0; i < D.Bones.Num(); ++i)
	{
		FConvJson B = FConvJson::Object();
		B.Set(TEXT("name"), Sk.Names[i]);
		B.Set(TEXT("original_name"), D.Bones[i].Name);
		B.Set(TEXT("parent"), i ? D.Bones[i].Parent : -1);
		FConvJson L = FConvJson::Array();
		L.Add(FConvJson::RoundTo(Sk.LocalPos[i].X, 5)); L.Add(FConvJson::RoundTo(Sk.LocalPos[i].Y, 5)); L.Add(FConvJson::RoundTo(Sk.LocalPos[i].Z, 5));
		B.Set(TEXT("location"), L);
		const FQ& Q = Sk.LocalRot[i];
		FConvJson Rq = FConvJson::Array();
		Rq.Add(FConvJson::RoundTo(Q.X, 7)); Rq.Add(FConvJson::RoundTo(Q.Y, 7)); Rq.Add(FConvJson::RoundTo(Q.Z, 7)); Rq.Add(FConvJson::RoundTo(Q.W, 7));
		B.Set(TEXT("rotation_quat"), Rq);
		FConvJson Wl = FConvJson::Array();
		for (int32 c = 0; c < 3; ++c) Wl.Add(FConvJson::RoundTo(Sk.World[i].T[c], 4));
		B.Set(TEXT("world_location"), Wl);
		Bones.Add(B);
	}
	Meta.Set(TEXT("bones"), Bones);
	Meta.Set(TEXT("sockets"), Sockets);
	FConvJson Mats = FConvJson::Array();
	for (const FString& M : MatNames) Mats.Add(M);
	Meta.Set(TEXT("materials"), Mats);
	Meta.Set(TEXT("sections"), SectionsJson(Prims));
	Meta.Set(TEXT("animation"), FConvJson::StrOrNull(AnimPath));
	Meta.Set(TEXT("animation_file"), FConvJson());
	Meta.Set(TEXT("lod_count"), D.Lods.Num());
	FConvJson Lods = FConvJson::Array();
	for (const FLod& L : D.Lods)
	{
		FConvJson Lj = FConvJson::Object();
		Lj.Set(TEXT("distance_factor"), double(L.DistanceFactor));
		Lj.Set(TEXT("points"), L.Points.Num() / 3);
		Lods.Add(Lj);
	}
	Meta.Set(TEXT("lods"), Lods);
	FConvJson Col = FConvJson::Object();
	Col.Set(TEXT("karma_props"), RefPathOrNull(*this, Obj->Pkg, D.KarmaProps));
	Col.Set(TEXT("collision_mesh"), RefPathOrNull(*this, Obj->Pkg, D.CollisionMesh));
	FConvJson Spheres = D.BoneSpheres, Boxes = D.BoneBoxes;
	for (FConvJson& S : Spheres.A)
	{
		for (FConvJson& V : S.Find(TEXT("offset"))->A) V = FConvJson(V.F * Xf.S);
		S.Set(TEXT("radius"), S.Get(TEXT("radius")).F * Xf.S);
	}
	for (FConvJson& B : Boxes.A)
	{
		for (FConvJson& V : B.Find(TEXT("offset"))->A) V = FConvJson(V.F * Xf.S);
		for (FConvJson& V : B.Find(TEXT("radii"))->A) V = FConvJson(V.F * Xf.S);
	}
	Col.Set(TEXT("bone_spheres"), Spheres);
	Col.Set(TEXT("bone_boxes"), Boxes);
	Col.Set(TEXT("note"), TEXT("offsets/radii scaled by bone_scale (bone space)"));
	Meta.Set(TEXT("collision"), Col);
	if (AnimRef.IsSet())
	{
		Meta.Set(TEXT("animation_file"), FConvJson::StrOrNull(ExportRef(AnimRef)));
	}
	WriteJson(FPaths::ChangeExtension(GPath, TEXT(".json")), Meta);
	Counts[TEXT("skeletalmeshes")]++;
	return Rel(GPath);
}

FString FConvExporter::ExportAnimation(FConvObject* Obj)
{
	const FString Path = OutPath(TEXT("animations"), Obj, TEXT(".gltf"));
	const FString MetaPath = FPaths::ChangeExtension(Path, TEXT(".json"));
	const FString What = FString::Printf(TEXT("%s (%s)"), *Obj->Path(), *Obj->ClassName());
	FAnim Anim;
	FString Err;
	if (!ReadAnimation(Obj, Anim, Err))
	{
		Error(What, Err);
		return FString();
	}
	// representative mesh (exporter.py _anim_representative)
	FConvObject* Rep = nullptr;
	FMesh RepD;
	{
		TArray<FConvObject*> Cands;
		TSet<FString> Have;
		if (const TArray<FString>* IndexPaths = AnimMeshIndex().Find(Obj->Path().ToLower()))
		{
			for (const FString& P : *IndexPaths)
			{
				if (FConvObject* O = Mgr.ResolvePath(P, TEXT("SkeletalMesh")))
				{
					Cands.Add(O);
					Have.Add(O->Path().ToLower());
				}
			}
		}
		for (int32 I : Obj->Pkg->ExportsOfClass({ TEXT("SkeletalMesh") }))
		{
			FConvObject* O = Obj->Pkg->GetObject(I);
			if (Have.Contains(O->Path().ToLower())) continue;
			FMesh D;
			if (!ReadSkeletalMesh(O, D, Err)) continue;
			if (D.Animation && Canon(Obj->Pkg->MakeRef(D.Animation)).ToLower() == Obj->Path().ToLower())
			{
				Cands.Add(O);
			}
		}
		TSet<FString> ANames;
		for (const auto& B : Anim.Bones) ANames.Add(B.Key.ToLower());
		if (Cands.Num() == 0)
		{
			for (int32 I : Obj->Pkg->ExportsOfClass({ TEXT("SkeletalMesh") }))
			{
				FConvObject* O = Obj->Pkg->GetObject(I);
				FMesh D;
				if (!ReadSkeletalMesh(O, D, Err)) continue;
				TSet<FString> Names;
				for (const FBone& B : D.Bones) Names.Add(B.Name.ToLower());
				if (ANames.Num() && ANames.Difference(Names).Num() == 0)
				{
					Cands.Add(O);
				}
			}
		}
		int32 BestA = -1, BestB = -1, BestC = 0;
		for (FConvObject* O : Cands)
		{
			FMesh D;
			FGeo G;
			if (!ReadSkeletalMesh(O, D, Err) || !MeshGeometry(D, G, Err)) continue;
			TSet<FString> Names;
			for (const FBone& B : D.Bones) Names.Add(B.Name.ToLower());
			const int32 A = ANames.Intersect(Names).Num();
			const int32 B = O->Pkg == Obj->Pkg ? 1 : 0;
			const int32 C = -Names.Difference(ANames).Num();
			if (!Rep || A > BestA || (A == BestA && (B > BestB || (B == BestB && C > BestC))))
			{
				Rep = O;
				RepD = MoveTemp(D);
				BestA = A; BestB = B; BestC = C;
			}
		}
	}
	FConvJson Seqs = FConvJson::Array();
	TArray<FConvObject*> NotifyObjs;
	for (int32 i = 0; i < Anim.Sequences.Num(); ++i)
	{
		const FSeq& S = Anim.Sequences[i];
		const FMove* M = i < Anim.Moves.Num() ? &Anim.Moves[i] : nullptr;
		const int32 Nf = S.NumFrames;
		const double Rate = S.Rate != 0 ? double(S.Rate) : 30.0;
		FConvJson Ns = FConvJson::Array();
		for (const FNotify& N : S.Notifies)
		{
			FConvJson E = FConvJson::Object();
			E.Set(TEXT("time"), FConvJson::RoundTo(N.Time, 6));
			E.Set(TEXT("frame"), FConvJson::RoundTo(double(N.Time) * Nf, 3));
			E.Set(TEXT("function"), N.Function);
			E.Set(TEXT("object"), RefPathOrNull(*this, Obj->Pkg, N.Object));
			if (N.Object)
			{
				if (FConvObject* No = Obj->Pkg->MakeRef(N.Object).Load())
				{
					E.Set(TEXT("object_class"), No->ClassName());
					E.Set(TEXT("object_properties"), JsonifyProps(No->Props()));
					NotifyObjs.Add(No);
				}
			}
			Ns.Add(E);
		}
		FConvJson Sj = FConvJson::Object();
		Sj.Set(TEXT("index"), i);
		Sj.Set(TEXT("name"), S.Name);
		FConvJson G = FConvJson::Array();
		for (const FString& Gr : S.Groups) G.Add(Gr);
		Sj.Set(TEXT("groups"), G);
		Sj.Set(TEXT("start_frame"), S.StartFrame);
		Sj.Set(TEXT("num_frames"), Nf);
		Sj.Set(TEXT("rate"), FConvJson::RoundTo(S.Rate, 6));
		Sj.Set(TEXT("duration"), Nf > 1 ? FConvJson::RoundTo((Nf - 1) / Rate, 6) : 0.0);
		Sj.Set(TEXT("loop_duration"), FConvJson::RoundTo(Nf / Rate, 6));
		Sj.Set(TEXT("notifies"), Ns);
		if (M)
		{
			FConvJson Rs = FConvJson::Array();
			for (int32 c = 0; c < 3; ++c) Rs.Add(FConvJson::RoundTo(double(M->RootSpeed[c]) * Opt.Scale, 4));
			Sj.Set(TEXT("root_speed"), Rs);
			Sj.Set(TEXT("track_time"), FConvJson::RoundTo(M->TrackTime, 6));
			int32 MaxRot = 0, MaxPos = 0;
			for (const FTrack& T : M->Tracks) { MaxRot = FMath::Max(MaxRot, T.Quat.Num() / 4); MaxPos = FMath::Max(MaxPos, T.Pos.Num() / 3); }
			FConvJson Kc = FConvJson::Object();
			Kc.Set(TEXT("max_rot"), MaxRot);
			Kc.Set(TEXT("max_pos"), MaxPos);
			Sj.Set(TEXT("key_counts"), Kc);
		}
		else
		{
			Sj.Set(TEXT("root_speed"), FConvJson());
			Sj.Set(TEXT("track_time"), FConvJson());
			Sj.Set(TEXT("key_counts"), FConvJson());
		}
		Seqs.Add(Sj);
	}
	FConvJson Meta = FConvJson::Object();
	Meta.Set(TEXT("type"), TEXT("MeshAnimation"));
	Meta.Set(TEXT("path"), Obj->Path());
	Meta.Set(TEXT("scale"), Opt.Scale);
	Meta.Set(TEXT("tool_version"), ToolVersion);
	Meta.Set(TEXT("gltf"), FConvJson());
	Meta.Set(TEXT("representative_mesh"), Rep ? FConvJson(Rep->Path()) : FConvJson());
	FConvJson RefBones = FConvJson::Array();
	for (const auto& B : Anim.Bones)
	{
		FConvJson Bj = FConvJson::Object();
		Bj.Set(TEXT("name"), B.Key);
		Bj.Set(TEXT("parent"), B.Value);
		RefBones.Add(Bj);
	}
	Meta.Set(TEXT("ref_bones"), RefBones);
	Meta.Set(TEXT("sequence_count"), Seqs.Num());
	Meta.Set(TEXT("sequences"), Seqs);
	Meta.Set(TEXT("compression"), TEXT("UE2 AnalogTrack: float quaternion/position key lists with per-key times (1 key = constant track); resampled to every frame"));
	Meta.Set(TEXT("note"), TEXT("glTF animation i is sequence i; keys at time = frame / rate. Root keys carry the baked mesh-to-actor transform of representative_mesh."));
	for (FConvObject* No : NotifyObjs)
	{
		TArray<FConvRef> Refs;
		CollectRefs(No->Props(), Refs);
		for (const FConvRef& R : Refs)
		{
			const FString Cn = R.ClassName();
			if (Cn != TEXT("Class") && Cn != TEXT("Package"))
			{
				ExportRef(R);
			}
		}
	}
	if (!Rep)
	{
		Meta.Set(TEXT("error"), TEXT("no SkeletalMesh uses this animation; not exported to glTF"));
		WriteJson(MetaPath, Meta);
		Counts[TEXT("animations")]++;
		return Rel(MetaPath);
	}
	if (Fresh(Obj, Path, FString(), AnimToolVersion))
	{
		return Rel(Path);
	}
	Meta.Set(TEXT("tool_version"), AnimToolVersion);
	const FMesh& D = RepD;
	const FXf Xf(D.MeshScale, D.MeshOrigin, D.RotOrigin, Opt.Scale);
	const FSkel Sk = FSkel::FromMesh(D, Xf);
	FConvGltf Gb(Opt.Scale);
	TArray<int32> Roots, Joints;
	Gb.AddSkeleton(Sk.Names, Sk.Parents, ToUEQuats(Sk.LocalRot), Sk.LocalPos, Roots, Joints);
	TArray<FMatrix44d> W;
	for (const FM4& M : Sk.World) W.Add(M.ToUE());
	const int32 Skin = Gb.AddSkin(W, Joints, Roots[0]);
	{
		const double* R0 = Sk.World[0].T;
		FConvPrim Tri;
		Tri.Positions = { (float)R0[0], (float)R0[1], (float)R0[2], (float)(R0[0] + 1), (float)R0[1], (float)R0[2], (float)R0[0], (float)(R0[1] + 1), (float)R0[2] };
		Tri.UVs = { 0, 0, 0, 0, 0, 0 };
		Tri.Joints.SetNumZeroed(12);
		Tri.Weights = { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 };
		Tri.Indices = { 0, 2, 1 };
		Tri.Material = TEXT("None");
		const int32 MNode = Gb.AddSkinnedMesh(TEXT("AnimSkeleton"), { Tri }, Skin);
		TSet<FString> UnmatchedAll;
		for (int32 i = 0; i < Anim.Sequences.Num(); ++i)
		{
			if (IsCancelled()) return FString();
			const FSeq& S = Anim.Sequences[i];
			TMap<int32, FChan> Chans;
			TArray<FString> Unmatched;
			const int32 Nf = SampleSequence(Anim, i, Sk, Xf, Chans, Unmatched);
			UnmatchedAll.Append(Unmatched);
			const double Rate = S.Rate != 0 ? double(S.Rate) : 30.0;
			TArray<float> Times;
			if (Nf == 1)
			{
				Times = { 0.f, (float)(1.0 / Rate) };
			}
			else
			{
				for (int32 f = 0; f < Nf; ++f) Times.Add((float)(f / Rate));
			}
			TArray<FConvGltf::FChannel> Channels;
			for (int32 Ji = 0; Ji < Sk.Names.Num(); ++Ji)
			{
				FConvGltf::FChannel Cr, Ct;
				Cr.Node = Ct.Node = Joints[Ji];
				Cr.bRotation = true;
				const FChan* C = Chans.Find(Ji);
				const int32 Reps = Nf == 1 ? 2 : 1;
				for (int32 r = 0; r < Reps; ++r)
				{
					for (int32 f = 0; f < Nf; ++f)
					{
						const FQ Q = C ? C->Rot[f] : Sk.LocalRot[Ji];
						const FVector3d P = C ? C->Pos[f] : Sk.LocalPos[Ji];
						Cr.Values.Append({ (float)Q.X, (float)Q.Y, (float)Q.Z, (float)Q.W });
						Ct.Values.Append({ (float)P.X, (float)P.Y, (float)P.Z });
					}
				}
				Channels.Add(MoveTemp(Cr));
				Channels.Add(MoveTemp(Ct));
			}
			FConvJson Ex = FConvJson::Object();
			Ex.Set(TEXT("num_frames"), Nf);
			Ex.Set(TEXT("rate"), double(S.Rate));
			Ex.Set(TEXT("sequence_index"), i);
			Gb.AddAnimation(S.Name, Times, Channels, Ex);
		}
		TArray<int32> SceneNodes = Roots;
		SceneNodes.Add(MNode);
		FConvJson Extra = FConvJson::Object();
		Extra.Set(TEXT("source"), Obj->Path());
		Extra.Set(TEXT("skeleton_from"), Rep->Path());
		Gb.WriteSkinned(Path, SceneNodes, Extra);
		Meta.Set(TEXT("gltf"), FPaths::GetCleanFilename(Path));
		FConvJson Sb = FConvJson::Array();
		for (const FString& N : Sk.Names) Sb.Add(N);
		Meta.Set(TEXT("skeleton_bones"), Sb);
		TArray<FString> Um = UnmatchedAll.Array();
		Um.Sort([](const FString& A, const FString& B) { return FCString::Strcmp(*A, *B) < 0; });
		FConvJson Uj = FConvJson::Array();
		for (const FString& N : Um) Uj.Add(N);
		Meta.Set(TEXT("unmatched_bones"), Uj);
		FConvJson Mt = FConvJson::Object();
		Mt.Set(TEXT("mesh_scale"), F3(D.MeshScale));
		Mt.Set(TEXT("mesh_origin"), F3(D.MeshOrigin));
		Mt.Set(TEXT("rot_origin"), I3(D.RotOrigin));
		Mt.Set(TEXT("bone_scale"), FConvJson::RoundTo(Xf.S, 6));
		Meta.Set(TEXT("mesh_transform"), Mt);
	}
	WriteJson(MetaPath, Meta);
	Counts[TEXT("animations")]++;
	return Rel(Path);
}

FString FConvExporter::ExportVertMesh(FConvObject* Obj)
{
	const FString GPath = OutPath(TEXT("vertmeshes"), Obj, TEXT(".gltf"));
	const FString What = FString::Printf(TEXT("%s (%s)"), *Obj->Path(), *Obj->ClassName());
	FMesh D;
	FString Err;
	if (!ReadVertMesh(Obj, D, Err))
	{
		Error(What, Err);
		return FString();
	}
	const TArray<FString> MatNames = MeshMaterialNames(*this, Obj, D);
	if (Fresh(Obj, GPath, FString(), AnimToolVersion))
	{
		return Rel(GPath);
	}
	const TArray<uint32>& Packed = D.Verts.Num() ? D.Verts : D.Verts2;
	const int32 Nv = D.VertCount, Nfr = D.FrameCount;
	if (Nv <= 0 || Nfr <= 0 || Packed.Num() < int64(Nv) * Nfr)
	{
		Error(What, FString::Printf(TEXT("VertMesh frame data size mismatch (%d verts x %d frames, %d packed)"), Nv, Nfr, Packed.Num()));
		return FString();
	}
	if (!D.Wedges.Num() || !D.Faces.Num())
	{
		Error(What, TEXT("VertMesh without wedges/faces"));
		return FString();
	}
	auto Frame = [&](int32 F, int32 V, double Out[3])
	{
		const uint32 P = Packed[F * Nv + V];
		int32 X = P & 0x7FF, Y = (P >> 11) & 0x7FF, Z = (P >> 22) & 0x3FF;
		if (X >= 1024) X -= 2048;
		if (Y >= 1024) Y -= 2048;
		if (Z >= 512) Z -= 1024;
		Out[0] = X; Out[1] = Y; Out[2] = Z;
	};
	const FXf Xf(D.MeshScale, D.MeshOrigin, D.RotOrigin, Opt.Scale);
	TArray<double> Bind;
	for (int32 v = 0; v < Nv; ++v)
	{
		double P[3], O[3];
		Frame(0, v, P);
		Xf.Point(P, O);
		Bind.Append({ O[0], O[1], O[2] });
	}
	FSkel Sk;
	Sk.Names.Add(TEXT("Root"));
	Sk.Parents.Add(-1);
	Sk.LocalRot.Add(FQ());
	Sk.LocalPos.Add(FVector3d::ZeroVector);
	for (int32 v = 0; v < Nv; ++v)
	{
		Sk.Names.Add(FString::Printf(TEXT("V%04d"), v));
		Sk.Parents.Add(0);
		Sk.LocalRot.Add(FQ());
		Sk.LocalPos.Add(FVector3d(Bind[v * 3], Bind[v * 3 + 1], Bind[v * 3 + 2]));
	}
	Sk.ComputeWorld();
	FGeo Geo;
	for (const FWedge& W : D.Wedges)
	{
		if (W.V >= Nv)
		{
			Error(What, TEXT("wedge vertex out of range"));
			return FString();
		}
		double P[3];
		Frame(0, W.V, P);
		Geo.Positions.Append({ P[0], P[1], P[2] });
		Geo.UVs.Append({ W.U, W.T });
		Geo.Joints.Append({ W.V + 1, 0, 0, 0 });
		Geo.Weights.Append({ 1.0, 0.0, 0.0, 0.0 });
	}
	for (const FFace& F : D.Faces)
	{
		Geo.Tris.Append({ (int32)F.W[0], (int32)F.W[1], (int32)F.W[2] });
		Geo.Mat.Add(F.Mat);
	}
	const TArray<FConvPrim> Prims = SectionPrims(Geo, Xf, MatNames, true);
	FConvGltf Gb(Opt.Scale);
	TArray<int32> Roots, Joints;
	Gb.AddSkeleton(Sk.Names, Sk.Parents, ToUEQuats(Sk.LocalRot), Sk.LocalPos, Roots, Joints);
	TArray<FMatrix44d> Wm;
	for (const FM4& M : Sk.World) Wm.Add(M.ToUE());
	const int32 Skin = Gb.AddSkin(Wm, Joints, Roots[0]);
	const int32 MNode = Gb.AddSkinnedMesh(Obj->Name(), Prims, Skin);
	FConvJson Seqs = FConvJson::Array();
	for (int32 i = 0; i < D.Sequences.Num(); ++i)
	{
		const FSeq& S = D.Sequences[i];
		const int32 A = S.StartFrame;
		const int32 N = FMath::Max(S.NumFrames, 1);
		TArray<int32> Fr;
		for (int32 k = 0; k < N; ++k) Fr.Add(FMath::Clamp(A + k, 0, Nfr - 1));
		const double Rate = S.Rate != 0 ? double(S.Rate) : 30.0;
		if (N == 1) { const int32 F0 = Fr[0]; Fr.Add(F0); }
		TArray<float> Times;
		for (int32 k = 0; k < Fr.Num(); ++k) Times.Add((float)(k / Rate));
		TArray<FConvGltf::FChannel> Channels;
		Channels.SetNum(Nv);
		for (int32 v = 0; v < Nv; ++v)
		{
			Channels[v].Node = Joints[v + 1];
			Channels[v].bRotation = false;
			Channels[v].Values.Reserve(Fr.Num() * 3);
		}
		for (int32 F : Fr)
		{
			for (int32 v = 0; v < Nv; ++v)
			{
				double P[3], O[3];
				Frame(F, v, P);
				Xf.Point(P, O);
				Channels[v].Values.Append({ (float)O[0], (float)O[1], (float)O[2] });
			}
		}
		FConvJson Ex = FConvJson::Object();
		Ex.Set(TEXT("num_frames"), N);
		Ex.Set(TEXT("rate"), double(S.Rate));
		Ex.Set(TEXT("start_frame"), A);
		Gb.AddAnimation(S.Name, Times, Channels, Ex);
		FConvJson Sj = FConvJson::Object();
		Sj.Set(TEXT("index"), i);
		Sj.Set(TEXT("name"), S.Name);
		FConvJson G = FConvJson::Array();
		for (const FString& Gr : S.Groups) G.Add(Gr);
		Sj.Set(TEXT("groups"), G);
		Sj.Set(TEXT("start_frame"), A);
		Sj.Set(TEXT("num_frames"), N);
		Sj.Set(TEXT("rate"), FConvJson::RoundTo(S.Rate, 6));
		Sj.Set(TEXT("duration"), N > 1 ? FConvJson::RoundTo((N - 1) / Rate, 6) : 0.0);
		Sj.Set(TEXT("loop_duration"), FConvJson::RoundTo(N / Rate, 6));
		FConvJson Ns = FConvJson::Array();
		for (const FNotify& No : S.Notifies)
		{
			FConvJson E = FConvJson::Object();
			E.Set(TEXT("time"), FConvJson::RoundTo(No.Time, 6));
			E.Set(TEXT("function"), No.Function);
			E.Set(TEXT("object"), RefPathOrNull(*this, Obj->Pkg, No.Object));
			Ns.Add(E);
		}
		Sj.Set(TEXT("notifies"), Ns);
		Seqs.Add(Sj);
	}
	TArray<int32> SceneNodes = Roots;
	SceneNodes.Add(MNode);
	FConvJson Extra = FConvJson::Object();
	Extra.Set(TEXT("source"), Obj->Path());
	Extra.Set(TEXT("vertex_animation"), true);
	Gb.WriteSkinned(GPath, SceneNodes, Extra);
	FConvJson Meta = FConvJson::Object();
	Meta.Set(TEXT("type"), TEXT("VertMesh"));
	Meta.Set(TEXT("path"), Obj->Path());
	Meta.Set(TEXT("scale"), Opt.Scale);
	Meta.Set(TEXT("tool_version"), AnimToolVersion);
	Meta.Set(TEXT("gltf"), FPaths::GetCleanFilename(GPath));
	Meta.Set(TEXT("frame_count"), Nfr);
	Meta.Set(TEXT("vertex_count"), Nv);
	Meta.Set(TEXT("triangle_count"), D.Faces.Num());
	Meta.Set(TEXT("bounds"), BoundsOf(Bind));
	FConvJson Mt = FConvJson::Object();
	Mt.Set(TEXT("mesh_scale"), F3(D.MeshScale));
	Mt.Set(TEXT("mesh_origin"), F3(D.MeshOrigin));
	Mt.Set(TEXT("rot_origin"), I3(D.RotOrigin));
	Mt.Set(TEXT("baked"), true);
	Meta.Set(TEXT("mesh_transform"), Mt);
	FConvJson Mats = FConvJson::Array();
	for (const FString& M : MatNames) Mats.Add(M);
	Meta.Set(TEXT("materials"), Mats);
	Meta.Set(TEXT("sections"), SectionsJson(Prims));
	Meta.Set(TEXT("skeleton"), TEXT("Root + one joint per vertex (V0000...); animations move the vertex joints"));
	Meta.Set(TEXT("sequences"), Seqs);
	WriteJson(FPaths::ChangeExtension(GPath, TEXT(".json")), Meta);
	Counts[TEXT("vertmeshes")]++;
	return Rel(GPath);
}

#endif // ARENA_CONV_BUILD
