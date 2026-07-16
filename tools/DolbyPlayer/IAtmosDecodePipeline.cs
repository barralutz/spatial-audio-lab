namespace DolbyPlayer;

internal interface IAtmosDecodePipeline : IAsyncDisposable {
    Exception? Failure { get; }
    bool IsCompleted { get; }
    Task RestartAsync(double mediaStartSeconds, CancellationToken cancellationToken);
    Task WaitForPrebufferAsync(double seconds, CancellationToken cancellationToken);
}
