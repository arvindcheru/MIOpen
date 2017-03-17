#include <mlopen/convolution.hpp>
#include <mlopen/util.hpp>

#if MLOPEN_USE_TINYGEMM
#include <mlopen/gemm.hpp>
#endif

namespace mlopen {

struct AutoEnableProfiling
{
    AutoEnableProfiling(Handle& x)
    : h(x)
    {
        prev_state = h.IsProfilingEnabled();
        h.EnableProfiling();
    }

    ~AutoEnableProfiling()
    {
        h.EnableProfiling(prev_state);
        h.ResetKernelTime();
    }
private:
    Handle& h;
    bool prev_state;
};

int ConvolutionDescriptor::FindFwdWinogradKernel(Handle& handle,
		const TensorDescriptor&			xDesc,
		const TensorDescriptor&			wDesc,
		const TensorDescriptor&			yDesc,
        WinogradKernelParams&           k_p,
        KernelInvoke&                   kernel) const {

    mlo_construct_winograd construct_params(1);
    construct_params.setStream(&handle);

    construct_params.setOutputDescFromMLDesc(yDesc);
    construct_params.setInputDescFromMLDesc(xDesc);
    construct_params.setWeightDescFromMLDesc(wDesc);

    construct_params.setConvDescr(pad_h, pad_w, u, v, upscalex, upscaley);

    if(construct_params.mloConstruct() != -1) { //TODO: be more graceful with the check for whether a config is supported by winograd
        std::string program_name = construct_params.getKernelFile(); 
        std::string kernel_name = construct_params.getKernelName(); 
        std::string parms = construct_params.getCompilerOptions();

        std::string network_config;
        construct_params.mloBuildConf_Key(network_config);

        const std::vector<size_t> & vld = construct_params.getLocalWkSize();
        const std::vector<size_t> & vgd = construct_params.getGlobalWkSize();

        kernel = handle.GetKernel("mlopenConvolutionFwdAlgoWinograd",
                network_config,
                program_name,
                kernel_name,
                vld,
                vgd,
                parms);

        int N, C, H, W, K, n_groups;
        construct_params.getCompiledInParameters(&N, &C, &H, &W, &K, &n_groups);
        k_p = std::make_tuple(N, C, H, W, K, n_groups);

        return 0;
    }
    else
        return -1;
}

int ConvolutionDescriptor::FindDirectKernel(Handle& handle,
		const TensorDescriptor&			xDesc,
		const TensorDescriptor&			wDesc,
		const TensorDescriptor&			yDesc,
        std::vector<KernelInvoke>&      kernels,
        bool                            exhaustiveSearch,
        int                             direction) const {

    mlo_construct_direct2D construct_params(direction); 
    construct_params.doSearch(exhaustiveSearch);
    construct_params.saveSearchRequest(true);

    construct_params.setGeneralCompOptions("");

    construct_params.setStream(&handle);

    construct_params.setOutputDescFromMLDesc(yDesc);
    construct_params.setInputDescFromMLDesc(xDesc);
    construct_params.setWeightDescFromMLDesc(wDesc);

    construct_params.setConvDescr(pad_h, pad_w, u, v, upscalex, upscaley);

    construct_params.mloConstruct();
    std::string program_name = construct_params.getKernelFile();
    std::string kernel_name = construct_params.getKernelName(); 
    std::string parms = construct_params.getCompilerOptions(); 

    std::string network_config;
    construct_params.mloBuildConf_Key(network_config);

    const std::vector<size_t> & vld = construct_params.getLocalWkSize();
    const std::vector<size_t> & vgd = construct_params.getGlobalWkSize();

    std::string algorithm = (direction == 1) ? "mlopenConvolutionFwdAlgoDirect"
                            : "mlopenConvolutionBwdDataAlgoDirect";

    // if not 11x11
	if (program_name.compare("MLOpenConvFwd_LxL_11.cl") != 0)
	{
        
		auto k = handle.GetKernel(algorithm,
			network_config,
			program_name,
			kernel_name,
			vld,
			vgd,
			parms);

        kernels.push_back(k);
	}
	else
	{
		const std::vector<mlo_kernel_info> & bwd_wrw_info = construct_params.getKernelsInfo();
		/*
		* get info for all kernels of the layer
		* std::string _kernel_name;
		* std::string _kernel_file;
		* std::string _comp_options;
		* std::vector<size_t> _g_wk;
		* std::vector<size_t> _l_wk;
		*/

		if (bwd_wrw_info.size() == 1)
		{
			const mlo_kernel_info &bwd_wrw = bwd_wrw_info[0];

		    auto k1 = handle.GetKernel(algorithm,
				network_config,
				std::get<1>(bwd_wrw),
				std::get<0>(bwd_wrw),
				std::get<4>(bwd_wrw),
				std::get<3>(bwd_wrw),
				std::get<2>(bwd_wrw));
            
            kernels.push_back(k1);
		}
		else
		{
			auto bwd_wrw_main = bwd_wrw_info[0];

			auto k1 = handle.GetKernel(algorithm,
				network_config,
				std::get<1>(bwd_wrw_main),
				std::get<0>(bwd_wrw_main),
				std::get<4>(bwd_wrw_main),
				std::get<3>(bwd_wrw_main),
				std::get<2>(bwd_wrw_main));

            kernels.push_back(k1);

			// second kernel hash
			network_config += "x1";
			// second pass  kernel
			auto bwd_wrw_red = bwd_wrw_info[1];

			auto k2 = handle.GetKernel(algorithm+"_pass2",
				network_config,
				std::get<1>(bwd_wrw_red),
				std::get<0>(bwd_wrw_red),
				std::get<4>(bwd_wrw_red),
				std::get<3>(bwd_wrw_red),
				std::get<2>(bwd_wrw_red));

            kernels.push_back(k2);
		}

	}

    return 0;
}

void ConvolutionDescriptor::FindConvFwdAlgorithm(Handle& handle,
        const TensorDescriptor&     xDesc,
        ConstData_t                 x,
        const TensorDescriptor&     wDesc,
        ConstData_t                 w,
        const TensorDescriptor&     yDesc,
        Data_t                      y,
        const int                   requestAlgoCount,
        int                         *returnedAlgoCount,
        mlopenConvAlgoPerf_t        *perfResults,
        Data_t                      workSpace,
        size_t                      workSpaceSize,
        bool                        exhaustiveSearch) const {

    if(x == nullptr || w == nullptr || y == nullptr) MLOPEN_THROW(mlopenStatusBadParm, "Buffers cannot be NULL");
    if(returnedAlgoCount == nullptr) MLOPEN_THROW(mlopenStatusBadParm, "returnedAlgoCount cannot be nullptr");
    if(perfResults == nullptr) MLOPEN_THROW(mlopenStatusBadParm, "perfResults cannot be nullptr");
    if(requestAlgoCount < 1) MLOPEN_THROW(mlopenStatusBadParm, "requestAlgoCount cannot be < 1");

    AutoEnableProfiling enableProfiling{handle};

    // create a dummy buffer for use as output for the kernel calls
    // because kernels are called purely for timing purposes
    auto tmp_y = handle.Create(yDesc.GetElementSize() * sizeof(yDesc.GetType()));
    
    // < algorith_name, <time, workspace_size> >
    std::vector< PerfField > perf_db;

    // GEMM based
    int in_n, in_c, in_h, in_w;
    std::tie(in_n, in_c, in_h, in_w) = tie4(xDesc.GetLengths());

    int wei_n, wei_h, wei_w;
    std::tie(wei_n, std::ignore, wei_h, wei_w) = tie4(wDesc.GetLengths());

    int out_h, out_w;
    std::tie(std::ignore, std::ignore, out_h, out_w) = tie4(yDesc.GetLengths());

    std::string network_config;
    std::string program_name;
    std::string kernel_name;
    std::string parms;

#if MLOPEN_USE_TINYGEMM
    size_t workspace_req = ForwardGetWorkSpaceSize(wDesc, yDesc);
    float time_gemm = 0;
    GemmGeometry gg = CreateGemmGeometryConvFwd(xDesc, wDesc, yDesc, false, network_config);

    // 1x1 does not require im2col or workspace
    if(wei_h == 1 && wei_w == 1) {
        gg.FindSolution(.003, handle, x, w, tmp_y.get(), false);
        gg.RunGemm(handle, x, w, tmp_y.get(), 0, 0, 0);

        time_gemm = in_n * handle.GetKernelTime();
        perf_db.push_back( PerfField{"mlopenConvolutionFwdAlgoGEMM", time_gemm, 0} );
    }

    // if not 1x1
    else if(workSpace != nullptr && workSpaceSize >= workspace_req) {
        float time_im2col = 0;
        size_t in_offset = 0;
        time_im2col = Im2ColGPU(handle, x, in_offset, in_c, in_h, in_w, wei_h, wei_w, out_h, out_w, pad_h, pad_w, v, u, workSpace);

        gg.FindSolution(.003, handle, workSpace, w, tmp_y.get(), false);
        gg.RunGemm(handle, workSpace, w, tmp_y.get(), 0, 0, 0);
        time_gemm = in_n * (time_im2col + handle.GetKernelTime());
        perf_db.push_back( PerfField{"mlopenConvolutionFwdAlgoGEMM", time_gemm, workspace_req} );
    }
#else
    (void)workSpace; // Suppress warning
    (void)workSpaceSize; // Suppress warning
#endif

    // Winograd algo
    float time_wino = 0;
    WinogradKernelParams k_p;
    KernelInvoke kernel_wino;
    if( FindFwdWinogradKernel(handle, xDesc, wDesc, yDesc, k_p, kernel_wino) == 0) { //TODO: be more graceful
        // Execute the winograd kernel
        int flags = 0;
        int reserved = 0;
        int *return_addr = nullptr;
        int N, C, H, W, K, n_groups;
        std::tie(N, C, H, W, K, n_groups) = k_p;
        kernel_wino (N, C, H, W, K, n_groups, flags, reserved, x, w, tmp_y.get(), return_addr);

        time_wino = handle.GetKernelTime();
        perf_db.push_back(PerfField{"mlopenConvolutionFwdAlgoWinograd", time_wino, 0});
    }

    // Direct algo
    float time_direct = 0;
    std::vector< KernelInvoke > kernel_direct;
    if( FindDirectKernel(handle, xDesc, wDesc, yDesc, kernel_direct, exhaustiveSearch, 1) == 0) { //Forward 

        // Execute the direct kernel
        float padding_val = 0;
        for(auto &k : kernel_direct) {
            k(x, w, tmp_y.get(), padding_val);
            time_direct += handle.GetKernelTime();
        }

        perf_db.push_back(PerfField{"mlopenConvolutionFwdAlgoDirect", time_direct, 0});
    }

    if(perf_db.empty())
        MLOPEN_THROW("Fwd Convolution cannot be executed due to incorrect params");

    // sort the perf_db
    std::sort(begin(perf_db), end(perf_db));

    // update perfResults
    *returnedAlgoCount = std::min(requestAlgoCount, static_cast<int>(perf_db.size()));

    for(int i = 0; i < *returnedAlgoCount; i++) {
        perfResults[i].fwd_algo = static_cast<mlopenConvFwdAlgorithm_t>(FwdAlgoResolver[ perf_db[i].name ]);
        perfResults[i].time = perf_db[i].time;
        perfResults[i].memory = perf_db[i].workspace;
    }
}

void ConvolutionDescriptor::ConvolutionForward(Handle& handle,
        const void                  * /*alpha*/,
        const TensorDescriptor&     xDesc,
        ConstData_t             x,
        const TensorDescriptor&     wDesc,
        ConstData_t             w,
        mlopenConvFwdAlgorithm_t    algo,
        const void                  * /*beta*/,
        const TensorDescriptor&     yDesc,
        Data_t                      y, 
        Data_t                      workSpace,
        size_t                      workSpaceSize) const {

    if(x == nullptr || w == nullptr || y == nullptr) {
        MLOPEN_THROW(mlopenStatusBadParm);
    }
    if(xDesc.GetSize() != yDesc.GetSize() || xDesc.GetSize() != wDesc.GetSize()) {
        MLOPEN_THROW(mlopenStatusBadParm);
    }
    if(xDesc.GetType() != yDesc.GetType() || xDesc.GetType() != wDesc.GetType()) {
        MLOPEN_THROW(mlopenStatusBadParm);
    }
    if(xDesc.GetLengths()[1] != wDesc.GetLengths()[1]) {
        MLOPEN_THROW(mlopenStatusBadParm);
    }
    if(xDesc.GetSize() < 3) {
        MLOPEN_THROW(mlopenStatusBadParm);
    }

    switch (algo)
    {
        case mlopenConvolutionFwdAlgoDirect:
        {
            // TODO(paul): Replicating code for now.
            mlo_construct_direct2D construct_params(1); // forward
            construct_params.setOutputDescFromMLDesc(yDesc);
            construct_params.setInputDescFromMLDesc(xDesc);
            construct_params.setWeightDescFromMLDesc(wDesc);
			construct_params.setConvDescr(pad_h, pad_w, u, v, upscalex, upscaley);
            construct_params.setStream(&handle);

            std::string network_config;
            construct_params.mloBuildConf_Key(network_config);

            std::string algorithm_name = "mlopenConvolutionFwdAlgoDirect";
            float padding_val = 0;
            auto kernel = handle.GetKernel(algorithm_name, network_config);


            // if not 11x11
            if ((kernel.GetName() != "MLOpenCvFwd11x11") )
            {

                kernel(x, w, y, padding_val);
            }
            else
            {
                construct_params.mloConstruct();

                const std::vector<mlo_kernel_info> & bwd_wrw_info = construct_params.getKernelsInfo();

                if (bwd_wrw_info.size() == 1)
                {
                    kernel(x, w, y, padding_val);
                }
                else
                {
                    // second kernel has
                    network_config += "x1";
                    auto kernel2 = handle.GetKernel(algorithm_name+"_pass2", network_config);

                    handle.ResetKernelTime();
                    kernel(x, w, y, padding_val);

                    float time0 = handle.GetKernelTime();
                    kernel2(x, w, y, padding_val);

                    handle.AccumKernelTime(time0);
                }
            }
        }
        break;

        case mlopenConvolutionFwdAlgoWinograd:
        {
            mlo_construct_winograd construct_params(1); // forward
            construct_params.setOutputDescFromMLDesc(yDesc);
            construct_params.setInputDescFromMLDesc(xDesc);
            construct_params.setWeightDescFromMLDesc(wDesc);

            construct_params.setStream(&handle);

            std::string network_config;
            construct_params.mloBuildConf_Key(network_config);

            std::string algorithm_name = "mlopenConvolutionFwdAlgoWinograd";
            auto kernel = handle.GetKernel(algorithm_name, network_config);

            int flags = 0;
            int reserved = 0;
            int *return_addr = nullptr;
            int N, C, H, W, K, n_groups;
            construct_params.getCompiledInParameters(&N, &C, &H, &W, &K, &n_groups);
            kernel(N, C, H, W, K, n_groups, flags, reserved, x, w, y, return_addr);
        }
        break;

        case mlopenConvolutionFwdAlgoGEMM:
        {
            int in_n, in_c, in_h, in_w;
            std::tie(in_n, in_c, in_h, in_w) = tie4(xDesc.GetLengths());

            int wei_n, wei_h, wei_w;
            std::tie(wei_n, std::ignore, wei_h, wei_w) = tie4(wDesc.GetLengths());

            int out_h, out_w;
            std::tie(std::ignore, std::ignore, out_h, out_w) = tie4(yDesc.GetLengths());

            if((wei_h != 1 && wei_w != 1) && 
                (workSpace == nullptr || workSpaceSize < ForwardGetWorkSpaceSize(wDesc, yDesc))) {
                MLOPEN_THROW("Workspace is required");
            }

            std::string network_config;
#if MLOPEN_USE_TINYGEMM
            CreateGemmGeometryConvFwd(xDesc, wDesc, yDesc, false, network_config);
            GemmGeometry gg = GetGemmGeometry("mlopenConvolutionFwdAlgoGEMM", network_config);

            float time_0 = 0;
            float t1 = 0;
            for(int i = 0; i < in_n; i++) {
                int out_offset = i * wei_n * out_h * out_w;
                if(wei_h != 1 && wei_w != 1) {
                    size_t in_offset = i * in_c * in_h * in_w;
                    Im2ColGPU(handle, x, in_offset, in_c, in_h, in_w, wei_h, wei_w, out_h, out_w, pad_h, pad_w, v, u, workSpace);
                    if(handle.IsProfilingEnabled())
                        t1 = handle.GetKernelTime();

                    gg.RunGemm(handle, workSpace, w, y, 0, 0, out_offset);

                    // Update times for both the kernels
                    if(handle.IsProfilingEnabled()) {
                        if(i == in_n - 1)
                            handle.AccumKernelTime(t1+time_0);
                        else
                            handle.AccumKernelTime(t1);
                        time_0 += handle.GetKernelTime();
                    }
                }
                else if(wei_h == 1 && wei_w == 1) {
                    int in_offset = i * in_c * in_h * in_w;
                    gg.RunGemm(handle, x, w, y, in_offset, 0, out_offset);
                    if(handle.IsProfilingEnabled()) {
                        if(i == in_n - 1)
                            handle.AccumKernelTime(time_0);
                        time_0 += handle.GetKernelTime();
                    }

                } 
            }
#else
            MLOPEN_THROW("GEMM is not supported");
#endif
        }
        break;
        case mlopenConvolutionFwdAlgoFFT:
            break;
    }
}

// FindBackwardDataAlgorithm()
//
void ConvolutionDescriptor::FindConvBwdDataAlgorithm(Handle& handle,
        const TensorDescriptor&     dyDesc,
        ConstData_t                 dy,
        const TensorDescriptor&     wDesc,
        ConstData_t                 w,
        const TensorDescriptor&     dxDesc,
        ConstData_t                 dx,
        const int                   requestAlgoCount,
        int                         *returnedAlgoCount,
        mlopenConvAlgoPerf_t        *perfResults,
        void                        * /*workSpace*/,
        size_t                      /* workSpaceSize*/,
        bool                        exhaustiveSearch) const {

    if(dx == nullptr || w == nullptr || dy == nullptr) MLOPEN_THROW(mlopenStatusBadParm, "Buffers cannot be NULL");
    if(returnedAlgoCount == nullptr) MLOPEN_THROW(mlopenStatusBadParm, "returnedAlgoCount cannot be nullptr");
    if(perfResults == nullptr) MLOPEN_THROW(mlopenStatusBadParm, "perfResults cannot be nullptr");
    if(requestAlgoCount < 1) MLOPEN_THROW(mlopenStatusBadParm, "requestAlgoCount cannot be < 1");

    // create a dummy buffer for use as output for the kernel calls
    // because kernels are called purely for timing purposes
    auto tmp_dx = handle.Create(dxDesc.GetElementSize() * sizeof(dxDesc.GetType()));

    AutoEnableProfiling enableProfiling{handle};

    // < algorith_name, <time, workspace_size> >
    std::vector< PerfField > perf_db;

    std::vector<KernelInvoke> kernel_direct;
    float time_direct = 0;
    if( FindDirectKernel(handle, dxDesc, wDesc, dyDesc, kernel_direct, exhaustiveSearch, 0) == 0) { //Backward
        float padding_val = 0;

        for(auto &k : kernel_direct) {
            k(dy, w, tmp_dx.get(), padding_val);
            time_direct += handle.GetKernelTime();
        }

        perf_db.push_back(PerfField{"mlopenConvolutionBwdDataAlgoDirect", time_direct, 0});
    }
    else
        MLOPEN_THROW(mlopenStatusUnknownError, "Backward Data Algo cannot be executed");

    // sorting not required because we implement only one algorithm

    // update perfResults
    *returnedAlgoCount = std::min(requestAlgoCount, static_cast<int>(perf_db.size()));

    for(int i = 0; i < *returnedAlgoCount; i++) {
        perfResults[i].bwd_data_algo = static_cast<mlopenConvBwdDataAlgorithm_t>(BwdDataAlgoResolver[ perf_db[i].name ]);
        perfResults[i].time = perf_db[i].time;
        perfResults[i].memory = perf_db[i].workspace;
    }

}

// BackwardDataAlgorithm()
void ConvolutionDescriptor::ConvolutionBackwardData(Handle& handle,
        const void                      * /*alpha*/,
        const TensorDescriptor&         dyDesc,
        ConstData_t                 dy,
        const TensorDescriptor&         wDesc,
        ConstData_t                 w,
        mlopenConvBwdDataAlgorithm_t    /* algo */,
        const void                      * /*beta*/,
        const TensorDescriptor&         dxDesc,
        Data_t                          dx, 
        void                            * /*workSpace*/,
        size_t                           /*workSpaceSize*/) const {

    if(dx == nullptr || w == nullptr || dy == nullptr) {
        MLOPEN_THROW(mlopenStatusBadParm);
    }
    if(dyDesc.GetSize() != dxDesc.GetSize() || dyDesc.GetSize() != wDesc.GetSize()) {
        MLOPEN_THROW(mlopenStatusBadParm);
    }
    if(dyDesc.GetType() != dxDesc.GetType() || dyDesc.GetType() != wDesc.GetType()) {
        MLOPEN_THROW(mlopenStatusBadParm);
    }
    if(dyDesc.GetLengths()[1] != wDesc.GetLengths()[0]) {
        MLOPEN_THROW(mlopenStatusBadParm);
    }
    if(dyDesc.GetSize() < 3) {
        MLOPEN_THROW(mlopenStatusBadParm);
    }

    // Launch all kernels and store the perf, workspace limits, etc.
    mlo_construct_direct2D construct_params(0); // backward
    {
        construct_params.setOutputDescFromMLDesc(dyDesc);
        construct_params.setInputDescFromMLDesc(dxDesc);
        construct_params.setWeightDescFromMLDesc(wDesc);
        construct_params.setStream(&handle);
    }

    std::string network_config;
    construct_params.mloBuildConf_Key(network_config);

    float padding_val = 0;
    handle.GetKernel("mlopenConvolutionBwdDataAlgoDirect", network_config) (dy, w, dx, padding_val);
}

// ConvolutionBackwardWeightsGetWorkSpaceSize
// FindBackwardWeightsAlgorithm()
//
void ConvolutionDescriptor::FindConvBwdWeightsAlgorithm(Handle& handle,
        const TensorDescriptor&     dyDesc,
        ConstData_t                 dy,
        const TensorDescriptor&     xDesc,
        ConstData_t                 x,
        const TensorDescriptor&     dwDesc,
        Data_t                      dw,
        const int                   requestAlgoCount,
        int                         *returnedAlgoCount,
        mlopenConvAlgoPerf_t        *perfResults,
        Data_t                      workSpace,
        size_t                      workSpaceSize,
        bool                        /*exhaustiveSearch*/) const {

    if(x == nullptr || dw == nullptr || dy == nullptr) MLOPEN_THROW(mlopenStatusBadParm, "Buffers cannot be NULL");
    if(returnedAlgoCount == nullptr) MLOPEN_THROW(mlopenStatusBadParm, "returnedAlgoCount cannot be nullptr");
    if(perfResults == nullptr) MLOPEN_THROW(mlopenStatusBadParm, "perfResults cannot be nullptr");
    if(requestAlgoCount < 1) MLOPEN_THROW(mlopenStatusBadParm, "requestAlgoCount cannot be < 1");

    // create a dummy buffer for use as output for the kernel calls
    // because kernels are called purely for timing purposes
    auto tmp_dw = handle.Create(dwDesc.GetElementSize() * sizeof(dwDesc.GetType()));

    AutoEnableProfiling enableProfiling{handle};

    // < algorith_name, <time, workspace_size> >
    std::vector< PerfField > perf_db;

    // GEMM based
    int in_n, in_c, in_h, in_w;
    std::tie(in_n, in_c, in_h, in_w) = tie4(xDesc.GetLengths());

    int wei_n, wei_h, wei_w;
    std::tie(wei_n, std::ignore, wei_h, wei_w) = tie4(dwDesc.GetLengths());

    int out_h, out_w;
    std::tie(std::ignore, std::ignore, out_h, out_w) = tie4(dyDesc.GetLengths());

    std::string network_config;
    size_t workspace_req = 0;

#if MLOPEN_USE_TINYGEMM
    GemmGeometry gg = CreateGemmGeometryConvBwdWeights(dyDesc, xDesc, dwDesc, false, network_config);
    workspace_req = BackwardWeightsGetWorkSpaceSizeGEMM(dyDesc, dwDesc);
    float time_gemm = 0;

    // 1x1 does not require im2col or workspace
    if(wei_h == 1 && wei_w == 1) {
        gg.FindSolution(.003, handle, x, dy, tmp_dw.get(), false);
        gg.RunGemm(handle, x, dy, tmp_dw.get(), 0, 0, 0);

        time_gemm = in_n * handle.GetKernelTime();
        perf_db.push_back( PerfField{"mlopenConvolutionBwdWeightsAlgoGEMM", time_gemm, 0} );
    }
    // if not 1x1
    else if(workSpace != nullptr && workSpaceSize >= workspace_req) {
        float time_im2col = 0;
        size_t in_offset = 0;
        time_im2col = Im2ColGPU(handle, x, in_offset, in_c, in_h, in_w, wei_h, wei_w, out_h, out_w, pad_h, pad_w, v, u, workSpace);

        gg.FindSolution(.003, handle, workSpace, dy, tmp_dw.get(), false);
        gg.RunGemm(handle, workSpace, dy, tmp_dw.get(), 0, 0, 0);
        time_gemm = in_n * (time_im2col + handle.GetKernelTime());
        perf_db.push_back( PerfField{"mlopenConvolutionBwdWeightsAlgoGEMM", time_gemm, workspace_req} );
    }
#endif

    (void)workSpace; // Suppress warning
    (void)workSpaceSize; // Suppress warning

    if(wei_w >= wei_h && !(in_h * in_w > (8 * 1024) && wei_w == wei_h && wei_w == 1))
    {
        mlo_construct_BwdWrW2D construct_params(0); // backward with regards to weights
        construct_params.doSearch(false);
        construct_params.setStream(&handle);
        construct_params.setOutputDescFromMLDesc(dyDesc);
        construct_params.setInputDescFromMLDesc(xDesc);
        construct_params.setWeightDescFromMLDesc(dwDesc);
        construct_params.setConvDescr(pad_h, pad_w, u, v, upscalex, upscaley);
        construct_params.mloConstruct();

        construct_params.mloBuildConf_Key(network_config);

        const std::vector<mlo_kernel_info> & bwd_wrw_info = construct_params.getKernelsInfo();
        /*
         * get info for all kernels of the layer
         * std::string _kernel_name;
         * std::string _kernel_file;
         * std::string _comp_options;
         * std::vector<size_t> _g_wk;
         * std::vector<size_t> _l_wk;
         */

        float time_direct = 0;
        if (bwd_wrw_info.size() == 1)
        {
            const mlo_kernel_info &bwd_wrw = bwd_wrw_info[0];
         //   float padding_val = 0;

            handle.GetKernel("mlopenConvolutionBwdWeightsAlgoDirect_Main",
                    network_config,
                    std::get<1>(bwd_wrw),
                    std::get<0>(bwd_wrw),
                    std::get<4>(bwd_wrw),
                    std::get<3>(bwd_wrw),
                    std::get<2>(bwd_wrw));
//                        (dy, x, tmp_dw.get(), padding_val);

            time_direct = handle.GetKernelTime();
            perf_db.push_back( PerfField{"mlopenConvolutionBwdWeightsAlgoDirect", time_direct, 0} );
        }
        else
        {
            workspace_req = BackwardWeightsGetWorkSpaceSizeDirect(dyDesc, xDesc, dwDesc);

            if(workSpace != nullptr && workSpaceSize >= workspace_req) {
                auto bwd_wrw_main = bwd_wrw_info[0];
       //         float padding_val = 0;

                handle.GetKernel("mlopenConvolutionBwdWeightsAlgoDirect_Main",
                        network_config,
                        std::get<1>(bwd_wrw_main),
                        std::get<0>(bwd_wrw_main),
                        std::get<4>(bwd_wrw_main),
                        std::get<3>(bwd_wrw_main),
                        std::get<2>(bwd_wrw_main));
//                    (dy, x, workSpace, padding_val);

                time_direct += handle.GetKernelTime();
            
                // second kernel hash
                network_config += "x1";
                // reduction  kernel
                auto bwd_wrw_red = bwd_wrw_info[1];

                handle.GetKernel("mlopenConvolutionBwdWeightsAlgoDirect_Red",
                        network_config,
                        std::get<1>(bwd_wrw_red),
                        std::get<0>(bwd_wrw_red),
                        std::get<4>(bwd_wrw_red),
                        std::get<3>(bwd_wrw_red),
                        std::get<2>(bwd_wrw_red));
  //                  (workSpace, tmp_dw.get());

                time_direct += handle.GetKernelTime();
                perf_db.push_back( PerfField{"mlopenConvolutionBwdWeightsAlgoDirect", time_direct, workspace_req} );
            }
        }
    }

    if(perf_db.empty())
        MLOPEN_THROW("Bwd Weights Convolution cannot be executed due to incorrect params");

    // sort the perf_db
    std::sort(begin(perf_db), end(perf_db));

    // update perfResults
    *returnedAlgoCount = std::min(requestAlgoCount, static_cast<int>(perf_db.size()));

    // TODO: Uncomment this block after direct/gemm algos are tested on hip
    // for(int i = 0; i < *returnedAlgoCount; i++) {
    //     perfResults[i].bwd_weights_algo = static_cast<mlopenConvBwdWeightsAlgorithm_t>(BwdWeightsAlgoResolver[ perf_db[i].name ]);
    //     perfResults[i].time = perf_db[i].time;
    //     perfResults[i].memory = perf_db[i].workspace;
    // }

#if MLOPEN_USE_TINYGEMM
    perfResults[0].bwd_weights_algo = mlopenConvolutionBwdWeightsAlgoGEMM;
#else
    perfResults[0].bwd_weights_algo = mlopenConvolutionBwdWeightsAlgoDirect;
#endif
}

// BackwardWeightsAlgorithm()
void ConvolutionDescriptor::ConvolutionBackwardWeights(Handle& handle,
        const void                      * /*alpha*/,
        const TensorDescriptor&         dyDesc,
        ConstData_t                     dy,
        const TensorDescriptor&         xDesc,
        ConstData_t                     x,
        mlopenConvBwdWeightsAlgorithm_t algo,
        const void                      * /*beta*/,
        const TensorDescriptor&         dwDesc,
        Data_t                          dw, 
        Data_t                          workSpace,
        size_t                          workSpaceSize) const {

    if(x == nullptr || dw == nullptr || dy == nullptr) {
        MLOPEN_THROW(mlopenStatusBadParm);
    }
    if(dyDesc.GetSize() != dwDesc.GetSize() || dyDesc.GetSize() != xDesc.GetSize()) {
        MLOPEN_THROW(mlopenStatusBadParm);
    }
    if(dyDesc.GetType() != dwDesc.GetType() || dyDesc.GetType() != xDesc.GetType()) {
        MLOPEN_THROW(mlopenStatusBadParm);
    }
    if(dyDesc.GetLengths()[0] != xDesc.GetLengths()[0]) {
        MLOPEN_THROW(mlopenStatusBadParm);
    }
    if(dyDesc.GetSize() < 3) {
        MLOPEN_THROW(mlopenStatusBadParm);
    }

    int in_n, in_c, in_h, in_w;
    std::tie(in_n, in_c, in_h, in_w) = tie4(xDesc.GetLengths());

    int wei_n, wei_h, wei_w;
    std::tie(wei_n, std::ignore, wei_h, wei_w) = tie4(dwDesc.GetLengths());

    int out_h, out_w;
    std::tie(std::ignore, std::ignore, out_h, out_w) = tie4(dyDesc.GetLengths());

    switch (algo)
    {
        case mlopenConvolutionBwdWeightsAlgoGEMM:
        {
            std::string network_config;

            if((wei_h != 1 && wei_w != 1) &&
                    (workSpace == nullptr || workSpaceSize < BackwardWeightsGetWorkSpaceSizeGEMM(dyDesc, dwDesc))) {
                MLOPEN_THROW("Workspace is required");
            }
#if MLOPEN_USE_TINYGEMM
            CreateGemmGeometryConvBwdWeights(dyDesc, xDesc, dwDesc, false, network_config);
            GemmGeometry gg = GetGemmGeometry("mlopenConvolutionBwdWeightsAlgoGEMM", network_config);

            handle.ResetKernelTime();
            float time_0 = 0;
            float t1 = 0;
            for(int i = 0; i < in_n; i++) {
                int out_offset = i * wei_n * out_h * out_w;
                if(wei_h != 1 && wei_w != 1) {
                    size_t in_offset = i * in_c * in_h * in_w;
                    Im2ColGPU(handle, x, in_offset, in_c, in_h, in_w, wei_h, wei_w, out_h, out_w, pad_h, pad_w, v, u, workSpace);
                    if(handle.IsProfilingEnabled())
                        t1 = handle.GetKernelTime();

                    gg.RunGemm(handle, workSpace, dy, dw, 0, out_offset, 0);

                    // Update times for both the kernels
                    if(handle.IsProfilingEnabled()) {
                        if(i == in_n - 1)
                            handle.AccumKernelTime(t1+time_0);
                        else
                            handle.AccumKernelTime(t1);
                        time_0 += handle.GetKernelTime();
                    }
                }
                else if(wei_h == 1 && wei_w == 1) {
                    int in_offset = i * in_c * in_h * in_w;
                    gg.RunGemm(handle, x, dy, dw, in_offset, out_offset, 0);

                    if(handle.IsProfilingEnabled()) {
                        if(i == in_n - 1)
                            handle.AccumKernelTime(time_0);
                        time_0 += handle.GetKernelTime();
                    }
                }
            }
#else
            MLOPEN_THROW("GEMM is not supported");
#endif
        }
        break;

        case mlopenConvolutionBwdWeightsAlgoDirect:
        {

			if (wei_w >= wei_h && !(in_h * in_w > (8 * 1024) && wei_w == wei_h && wei_w == 1))
			{
                mlo_construct_BwdWrW2D construct_params(0); // backward with regards to weights
                construct_params.setOutputDescFromMLDesc(dyDesc);
                construct_params.setInputDescFromMLDesc(xDesc);
                construct_params.setWeightDescFromMLDesc(dwDesc);
                construct_params.mloConstruct();

                std::string network_config;
                construct_params.mloBuildConf_Key(network_config);
                const std::vector<mlo_kernel_info> & bwd_wrw_info = construct_params.getKernelsInfo();

                handle.ResetKernelTime();

                // main kernel
                if (bwd_wrw_info.size() == 1)
                {
                    float padding_val = 0;
                    handle.GetKernel("mlopenConvolutionBwdWeightsAlgoDirect_Main",
                            network_config) (dy, x, dw, padding_val);
                }
                else
                {
                    if(workSpace == nullptr || workSpaceSize < BackwardWeightsGetWorkSpaceSizeDirect(dyDesc, xDesc, dwDesc)) {
                        MLOPEN_THROW("Workspace is requried");
                    }

                    float padding_val = 0;
                    handle.GetKernel("mlopenConvolutionBwdWeightsAlgoDirect_Main",
                            network_config) (dy, x, workSpace, padding_val);

                    float time0 = handle.GetKernelTime();
                    // second kernel has
                    network_config += "x1";
                    // reduction  kernel
                    handle.GetKernel("mlopenConvolutionBwdWeightsAlgoDirect_Red",
                            network_config) (workSpace, dw);

                    handle.AccumKernelTime(time0);
                }
            }
        }
        break;
    };
}

void ConvolutionBackwardBias(Handle& handle,
                                                    const void              * /*alpha*/,
                                                    const TensorDescriptor& dyDesc,
                                                    ConstData_t             dy,
                                                    const void              * /*beta*/,
                                                    const TensorDescriptor& dbDesc,
                                                    Data_t                  db) {
    if(dy == nullptr || db == nullptr) {
        MLOPEN_THROW(mlopenStatusBadParm);
    }
    if(dyDesc.GetLengths()[1] != dbDesc.GetLengths()[1]) {
        MLOPEN_THROW(mlopenStatusBadParm);
    }

    int out_n, out_c, out_h, out_w, stride_n, stride_c, stride_h, stride_w;
    std::tie(out_n, out_c, out_h, out_w) = tie4(dyDesc.GetLengths());
    std::tie(stride_n, stride_c, stride_h, stride_w) = tie4(dyDesc.GetStrides());
    std::string program_name = "MLOpenConvBwdBias.cl";
    std::string kernel_name = "MLOpenConvBwdB";

    std::string params;
    size_t lcl_grp_size0 = 256;
    size_t lcl_grp_size1 = 1;
    size_t local_mem_sz = 256;

    size_t map_size = out_w * out_h;
    size_t read_unit = 4;
    size_t map_size_aligned = (map_size + (read_unit - 1)) / read_unit;
    size_t off_pix = map_size - (map_size / read_unit) * read_unit;

    params = " -DMLO_CONVBWD_GROUP_SZ0=" + std::to_string(lcl_grp_size0);
    params += " -DMLO_CONVBWD_GROUP_SZ1=" + std::to_string(lcl_grp_size1);
    params += " -DMLO_CONVBWDB_LCL_MEMSZ=" + std::to_string(local_mem_sz);
    params += " -DMLO_CONVBWDB_UNITSIZE=" + std::to_string(read_unit);
    params += " -DMLO_OUT_WIDTH=" + std::to_string(out_w);
    params += " -DMLO_OUT_HEIGHT=" + std::to_string(out_h);
    params += " -DMLO_OUT_BATCH_SZ=" + std::to_string(out_n);
    params += " -DMLO_OUT_CHANNEL_STRIDE=" + std::to_string(stride_c);
    params += " -DMLO_OUT_BATCH_STRIDE=" + std::to_string(stride_n);
    params += " -DMLO_WK_SIZE=" + std::to_string(map_size_aligned);
    params += " -DMLO_N_PIX_OFF=" + std::to_string(off_pix);

    const std::vector<size_t> vld = {lcl_grp_size0, size_t{1}, size_t{1}};
    const std::vector<size_t> vgd = {lcl_grp_size0, static_cast<size_t>(out_c), size_t{1}};

    handle.GetKernel("mlopenConvolutionBwdBias",
            "",
            program_name,
            kernel_name,
            vld,
            vgd,
            params)(dy, db);

}

}  // namespace mlopen
