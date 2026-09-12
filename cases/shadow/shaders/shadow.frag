#version 450

// 阴影通道把片元的窗口深度写进颜色附件。深度测试由同一子通道的深度附件完成，
// 写入这里的是通过测试的那个背面的深度
layout(location = 0) out float outDepth;

void main()
{
    outDepth = gl_FragCoord.z;
}
