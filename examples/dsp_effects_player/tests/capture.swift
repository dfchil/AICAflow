import AVFoundation
import CoreMedia

final class Recorder: NSObject, AVCaptureAudioDataOutputSampleBufferDelegate {
    var first = true
    func captureOutput(_ output: AVCaptureOutput, didOutput sample: CMSampleBuffer, from connection: AVCaptureConnection) {
        guard let description = CMSampleBufferGetFormatDescription(sample) else { return }
        guard let asbd = CMAudioFormatDescriptionGetStreamBasicDescription(description) else { return }
        let flags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved
        precondition(asbd.pointee.mSampleRate == 48000 && asbd.pointee.mChannelsPerFrame == 4 &&
                     asbd.pointee.mBitsPerChannel == 32 && asbd.pointee.mFormatFlags == flags,
                     "Expected native 48 kHz, four-channel planar Float32 HDMI audio")
        let size = MemoryLayout<AudioBufferList>.size + 3 * MemoryLayout<AudioBuffer>.size
        let storage = UnsafeMutableRawPointer.allocate(byteCount: size, alignment: MemoryLayout<AudioBufferList>.alignment)
        defer { storage.deallocate() }
        let list = storage.bindMemory(to: AudioBufferList.self, capacity: 1)
        var block: CMBlockBuffer?
        let result = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(sample,
            bufferListSizeNeededOut: nil, bufferListOut: list, bufferListSize: size,
            blockBufferAllocator: kCFAllocatorDefault, blockBufferMemoryAllocator: kCFAllocatorDefault,
            flags: 0, blockBufferOut: &block)
        precondition(result == noErr, "Cannot read native HDMI buffers")
        let buffers = UnsafeMutableAudioBufferListPointer(list)
        precondition(buffers.count == 4)
        let frames = CMSampleBufferGetNumSamples(sample)
        if first { FileHandle.standardError.write(Data("HDMI: native channels 0/1, no downmix\n".utf8)); first = false }
        var data = [Int16](repeating: 0, count: frames * 2)
        for i in 0..<frames {
            for c in 0..<2 {
                let value = buffers[c].mData!.assumingMemoryBound(to: Float.self)[i]
                data[i * 2 + c] = Int16(max(-32768, min(32767, value * 32768)))
            }
        }
        data.withUnsafeBytes { FileHandle.standardOutput.write(Data($0)) }
    }
}
let devices = AVCaptureDevice.DiscoverySession(deviceTypes: [.microphone, .external], mediaType: .audio, position: .unspecified).devices
guard let device = devices.first(where: { $0.localizedName.contains("Live Gamer") }) else { fatalError("HDMI device missing") }
let session = AVCaptureSession()
FileHandle.standardError.write(Data("HDMI authorization: \(AVCaptureDevice.authorizationStatus(for: .audio).rawValue)\n".utf8))
session.addInput(try AVCaptureDeviceInput(device: device))
let output = AVCaptureAudioDataOutput()
let recorder = Recorder()
output.setSampleBufferDelegate(recorder, queue: DispatchQueue(label: "hdmi.audio"))
session.addOutput(output)
session.startRunning()
FileHandle.standardError.write(Data("HDMI running: \(session.isRunning)\n".utf8))
RunLoop.main.run()
