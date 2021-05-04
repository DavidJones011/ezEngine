#include <RendererVulkanPCH.h>

#include <RendererVulkan/Device/DeviceVulkan.h>
#include <RendererVulkan/Resources/BufferVulkan.h>
#include <RendererVulkan/Resources/TextureVulkan.h>
#include <RendererVulkan/Resources/UnorderedAccessViewVulkan.h>

#include <d3d11.h>

bool IsArrayView(const ezGALTextureCreationDescription& texDesc, const ezGALUnorderedAccessViewCreationDescription& viewDesc)
{
  return texDesc.m_uiArraySize > 1 || viewDesc.m_uiFirstArraySlice > 0;
}

ezGALUnorderedAccessViewVulkan::ezGALUnorderedAccessViewVulkan(
  ezGALResourceBase* pResource, const ezGALUnorderedAccessViewCreationDescription& Description)
  : ezGALUnorderedAccessView(pResource, Description)
  , m_pDXUnorderedAccessView(nullptr)
{
}

ezGALUnorderedAccessViewVulkan::~ezGALUnorderedAccessViewVulkan() {}

ezResult ezGALUnorderedAccessViewVulkan::InitPlatform(ezGALDevice* pDevice)
{
  // TODO
#if 0
  const ezGALTexture* pTexture = nullptr;
  if (!m_Description.m_hTexture.IsInvalidated())
    pTexture = pDevice->GetTexture(m_Description.m_hTexture);

  const ezGALBuffer* pBuffer = nullptr;
  if (!m_Description.m_hBuffer.IsInvalidated())
    pBuffer = pDevice->GetBuffer(m_Description.m_hBuffer);

  if (pTexture == nullptr && pBuffer == nullptr)
  {
    ezLog::Error("No valid texture handle or buffer handle given for unordered access view creation!");
    return EZ_FAILURE;
  }


  ezGALResourceFormat::Enum ViewFormat = m_Description.m_OverrideViewFormat;

  if (pTexture)
  {
    const ezGALTextureCreationDescription& TexDesc = pTexture->GetDescription();

    if (ViewFormat == ezGALResourceFormat::Invalid)
      ViewFormat = TexDesc.m_Format;
  }

  ezGALDeviceVulkan* pDXDevice = static_cast<ezGALDeviceVulkan*>(pDevice);


  DXGI_FORMAT DXViewFormat = DXGI_FORMAT_UNKNOWN;
  if (ezGALResourceFormat::IsDepthFormat(ViewFormat))
  {
    DXViewFormat = pDXDevice->GetFormatLookupTable().GetFormatInfo(ViewFormat).m_eDepthOnlyType;
  }
  else
  {
    DXViewFormat = pDXDevice->GetFormatLookupTable().GetFormatInfo(ViewFormat).m_eResourceViewType;
  }

  if (DXViewFormat == DXGI_FORMAT_UNKNOWN)
  {
    ezLog::Error("Couldn't get valid DXGI format for resource view! ({0})", ViewFormat);
    return EZ_FAILURE;
  }

  D3D11_UNORDERED_ACCESS_VIEW_DESC DXUAVDesc;
  DXUAVDesc.Format = DXViewFormat;

  ID3D11Resource* pDXResource = nullptr;

  if (pTexture)
  {
    pDXResource = static_cast<const ezGALTextureVulkan*>(pTexture->GetParentResource())->GetDXTexture();
    const ezGALTextureCreationDescription& texDesc = pTexture->GetDescription();

    const bool bIsArrayView = IsArrayView(texDesc, m_Description);

    switch (texDesc.m_Type)
    {
      case ezGALTextureType::Texture2D:
      case ezGALTextureType::Texture2DProxy:

        if (!bIsArrayView)
        {
          DXUAVDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
          DXUAVDesc.Texture2D.MipSlice = m_Description.m_uiMipLevelToUse;
        }
        else
        {
          DXUAVDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
          DXUAVDesc.Texture2DArray.MipSlice = m_Description.m_uiMipLevelToUse;
          DXUAVDesc.Texture2DArray.ArraySize = m_Description.m_uiArraySize;
          DXUAVDesc.Texture2DArray.FirstArraySlice = m_Description.m_uiFirstArraySlice;
        }
        break;

      case ezGALTextureType::Texture3D:

        DXUAVDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
        DXUAVDesc.Texture3D.MipSlice = m_Description.m_uiMipLevelToUse;
        DXUAVDesc.Texture3D.FirstWSlice = m_Description.m_uiFirstArraySlice;
        DXUAVDesc.Texture3D.WSize = m_Description.m_uiArraySize;
        break;

      default:
        EZ_ASSERT_NOT_IMPLEMENTED;
        return EZ_FAILURE;
    }
  }
  else if (pBuffer)
  {
    pDXResource = static_cast<const ezGALBufferVulkan*>(pBuffer)->GetDXBuffer();

    if (pBuffer->GetDescription().m_bUseAsStructuredBuffer)
      DXUAVDesc.Format = DXGI_FORMAT_UNKNOWN;

    DXUAVDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    DXUAVDesc.Buffer.FirstElement = m_Description.m_uiFirstElement;
    DXUAVDesc.Buffer.NumElements = m_Description.m_uiNumElements;
    DXUAVDesc.Buffer.Flags = 0;
    if (m_Description.m_bRawView)
      DXUAVDesc.Buffer.Flags |= D3D11_BUFFER_UAV_FLAG_RAW;
    if (m_Description.m_bAppend)
      DXUAVDesc.Buffer.Flags |= D3D11_BUFFER_UAV_FLAG_APPEND;
  }

  if (FAILED(pDXDevice->GetDXDevice()->CreateUnorderedAccessView(pDXResource, &DXUAVDesc, &m_pDXUnorderedAccessView)))
  {
    return EZ_FAILURE;
  }
  else
  {
    return EZ_SUCCESS;
  }
#endif

  return EZ_SUCCESS;
}

ezResult ezGALUnorderedAccessViewVulkan::DeInitPlatform(ezGALDevice* pDevice)
{
  // TODO
  return EZ_SUCCESS;
}

EZ_STATICLINK_FILE(RendererVulkan, RendererVulkan_Resources_Implementation_UnorderedAccessViewVulkan);