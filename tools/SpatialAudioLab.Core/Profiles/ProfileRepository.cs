using System.Text;

namespace SpatialAudioLab.Core.Profiles;

public sealed record ProfileListEntry(
    string Path,
    ProfileDocument? Profile,
    string? Error);

public sealed class ProfileRepository
{
    private static readonly UTF8Encoding Utf8WithoutBom = new(false);
    private readonly string profilesRoot;
    private readonly string activeProfilePath;

    public ProfileRepository(string profilesRoot)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(profilesRoot);
        this.profilesRoot = Path.GetFullPath(profilesRoot);
        activeProfilePath = Path.Combine(this.profilesRoot, "active-profile.txt");
        Directory.CreateDirectory(this.profilesRoot);
    }

    public IReadOnlyList<ProfileListEntry> List()
    {
        List<ProfileListEntry> profiles = [];
        foreach (string path in Directory.EnumerateFiles(
                     profilesRoot,
                     "*.ini",
                     SearchOption.TopDirectoryOnly).Order(StringComparer.OrdinalIgnoreCase))
        {
            try
            {
                profiles.Add(new ProfileListEntry(
                    path,
                    ProfileIniSerializer.Load(path),
                    null));
            }
            catch (Exception error) when (
                error is IOException or UnauthorizedAccessException or InvalidDataException)
            {
                profiles.Add(new ProfileListEntry(path, null, error.Message));
            }
        }

        return profiles;
    }

    public ProfileDocument? LoadActive()
    {
        if (!File.Exists(activeProfilePath) ||
            !Guid.TryParse(File.ReadAllText(activeProfilePath, Encoding.UTF8).Trim(), out Guid id))
        {
            return null;
        }

        string path = ProfilePath(id);
        if (!File.Exists(path))
        {
            return null;
        }

        try
        {
            ProfileDocument profile = ProfileIniSerializer.Load(path);
            return profile.Id == id ? profile : null;
        }
        catch (Exception error) when (
            error is IOException or UnauthorizedAccessException or InvalidDataException)
        {
            return null;
        }
    }

    public void Save(ProfileDocument profile)
    {
        ArgumentNullException.ThrowIfNull(profile);
        profile.Validate();
        WriteAtomic(ProfilePath(profile.Id), ProfileIniSerializer.Serialize(profile));
    }

    public void SetActive(Guid id)
    {
        if (id == Guid.Empty || !File.Exists(ProfilePath(id)))
        {
            throw new FileNotFoundException("The selected profile does not exist.", ProfilePath(id));
        }

        WriteAtomic(activeProfilePath, $"{id:D}\n");
    }

    public void Delete(Guid id)
    {
        File.Delete(ProfilePath(id));
        if (File.Exists(activeProfilePath) &&
            Guid.TryParse(File.ReadAllText(activeProfilePath, Encoding.UTF8).Trim(), out Guid active) &&
            active == id)
        {
            File.Delete(activeProfilePath);
        }
    }

    private string ProfilePath(Guid id) => Path.Combine(profilesRoot, $"{id:D}.ini");

    private static void WriteAtomic(string path, string contents)
    {
        string tempPath = $"{path}.tmp";
        try
        {
            using (FileStream stream = new(
                       tempPath,
                       FileMode.Create,
                       FileAccess.Write,
                       FileShare.None,
                       4096,
                       FileOptions.WriteThrough))
            using (StreamWriter writer = new(stream, Utf8WithoutBom, 4096, leaveOpen: true))
            {
                writer.Write(contents);
                writer.Flush();
                stream.Flush(flushToDisk: true);
            }

            File.Move(tempPath, path, overwrite: true);
        }
        finally
        {
            if (File.Exists(tempPath))
            {
                File.Delete(tempPath);
            }
        }
    }
}
