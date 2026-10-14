from struct import unpack

from math import *
from sys import *

def create_file_record(name: str, data: bytes, attr: int=0xff, creation_date: int=0, modification_date: int=1, path = []):
    if modification_date<creation_date:
        print("cfr: modifcation date is later than creation date")
    if len(name)>16:
        print(f"cfr: name too big, cutting to {str[:16]}")
        name=name[:16]
    metasector = bytearray(512)
    metasector[0] = 0x80                            #file
    metasector[1] = attr                            #attributes (todo: figure out)

    name = name.encode('ascii')

    length_in_sectors = ceil(len(data)/512)

    datasector = bytearray(512*length_in_sectors)

    for i in range(0,len(name)):
        metasector[i+2]=name[i]
    for i in range(0,4):
        metasector[i+18]=length_in_sectors.to_bytes(4,'little')[i]

    path_c=0

    for i in range(0,len(path)*2,2):
        metasector[i+22]=path[i//2].to_bytes(2,'little')[0]
        metasector[i+23]=path[i//2].to_bytes(2,'little')[1]
    
    for i in range(0,len(data)):
        datasector[i]=data[i]
    
    return metasector+datasector

def place_record(data_dest: bytes, data_src: bytes, sector_pos: int):
    dd=data_dest
    ds=data_src
    if dd[sector_pos*512]!=0x00:
        print("pr: overwriting already existing data")
    for i in range(sector_pos*512,len(data_src)+sector_pos*512):
        dd[i] = ds[i-sector_pos*512]
    return dd



def create_partition(part_name:str, size_in_sectors: int, attr: int=0xff, creation_date: int=0):
    if len(part_name)>16:
        print(f"cfr: partition name too big, cutting to {str[:16]}")
        part_name=part_name[:16]
    metasector = bytearray(512)
    metasector[0] = ord('M')
    metasector[1] = ord('C')
    metasector[2] = ord('F')
    metasector[3] = ord('S')
    part_name = part_name.encode()

    for i in range(0,len(part_name)):
        metasector[i+4]=part_name[i]
    for i in range(0,8):
        metasector[i+20]=size_in_sectors.to_bytes(8,'little')[i]
    datasector = bytearray(512*size_in_sectors)
    return metasector+datasector

def read_file_record(metasector: bytes) -> dict:
    if len(metasector) != 512:
        raise ValueError("Invalid metasector size")

    if metasector[0] != 0x80:
        raise ValueError("Not a file record (invalid type byte)")

    name = metasector[2:18].decode('ascii').rstrip('\x00')
    attr = metasector[1]
    
    length_bytes = metasector[18:22]
    length_in_sectors = int.from_bytes(byteorder='little',bytes=length_bytes)

    path = metasector[22:512]
    
    return {
        'type': 'file',
        'name': name.strip(),
        'attributes': attr,
        'size_sectors': length_in_sectors,
        'path': [path[i:i+2] for i in range(0, len(path), 2)]
    }

def read_partition_record(metasector: bytes) -> dict:
    if len(metasector) != 512:
        raise ValueError("Invalid partition metasector size")

    if metasector[:4] != b'MCFS':
        raise ValueError("Not a partition record (invalid signature)")

    name = metasector[4:20].decode('ascii').rstrip('\x00')
    size_bytes = metasector[20:28]
    size_sectors = int.from_bytes(byteorder='little',bytes=size_bytes)

    return {
        'type': 'partition',
        'name': name.strip(),
        'size': size_sectors
    }

def read_data_sector(data: bytes, sector_num: int) -> bytes:
    sector_start = sector_num * 512
    sector_end = sector_start + 512

    if sector_end > len(data):
        raise IndexError("Sector out of bounds")

    return data[sector_start:sector_end]

def validate_file_data(file_data: bytes, expected_sectors: int):
    total_size = len(file_data)
    required_size = expected_sectors * 512

    if total_size != required_size:
        raise ValueError(f"Data size mismatch: expected {required_size} bytes, got {total_size}")

def check_partition_integrity(partition: bytes):
    metasector = partition[:512]
    data_size = len(partition) - 512

    read_partition_record(metasector)  # Проверка мета-сектора

    expected_size = (read_partition_record(metasector)['size'] + 1) * 512  # +1 за мета-сектор
    if data_size != expected_size - 512:  # Вычитаем мета-сектор
        raise ValueError("Partition data size mismatch")

def check_file_integrity(file_data: bytes):
    metasector = file_data[:512]
    data_part = file_data[512:]

    file_info = read_file_record(metasector)
    validate_file_data(data_part, file_info['size_sectors'])

class PartitionManager:
    def __init__(self, total_sectors):
        self.total_sectors = total_sectors
        self.free_sectors = [(0, total_sectors - 1)]  # Список свободных диапазонов
        self.data = bytearray(total_sectors * 512)    # Хранилище данных раздела

    def _find_free_space(self, needed_sectors):
        """Поиск свободного места"""
        for i, (start, end) in enumerate(self.free_sectors):
            if end - start + 1 >= needed_sectors:
                return i+1, start+1
        return None, None

    def allocate(self, needed_sectors):
        """Выделение секторов с учетом занятых областей"""
        idx, start = self._find_free_space(needed_sectors)
        if idx is None:
            raise MemoryError("Нет места для файла")

        end = start + needed_sectors - 1
        new_free = []
        for i, (s, e) in enumerate(self.free_sectors):
            if i == idx:
                # Если область больше, чем нужно - оставшуюся часть добавляем обратно
                if end < e:
                    new_free.append((end + 1, e))
            else:
                new_free.append((s, e))
        self.free_sectors = new_free

        return start

    def add_file(self, name, data, attr=0xff):
        """Добавление файла с корректной записью"""
        data_size = len(data)
        data_sectors = (data_size + 511) // 512
        total_sectors = data_sectors + 1  # +1 мета-сектор файла

        start_sector = self.allocate(total_sectors)

        # Создаем мета-сектор файла
        file_data = create_file_record(name, data, attr)
        # Записываем в раздел
        start_byte = start_sector * 512
        end_byte = start_byte + len(file_data)
        
        return start_sector

    def get_file(self, sector_start):
        """Чтение файла по начальному сектору"""
        metasector = self.data[sector_start*512 : (sector_start+1)*512]
        file_info = read_file_record(metasector)
        
        data_sectors = file_info['size_sectors']
        data_start = sector_start + 1
        data_end = data_start + data_sectors - 1
        
        data = bytearray()
        for s in range(data_start, data_end+1):
            sector_data = self.data[s*512 : (s+1)*512]
            data += sector_data
        
        return {
            'info': file_info,
            'data': data
        }

    def list_files(self):
        """Просмотр всех файлов в разделе"""
        files = []
        for sector in range(self.total_sectors):
            if sector % 512 == 0:  # Проверяем только мета-секторы
                try:
                    metasector = self.data[sector*512 : (sector+1)*512]
                    if metasector[0] == 0x80:
                        file_info = read_file_record(metasector)
                        files.append({
                            'sector': sector,
                            'name': file_info['name'],
                            'size': file_info['size_sectors']
                        })
                except:
                    pass  # Не мета-сектор
        return files
    
def validate_full_disk_image(image_data):
    """
    Проверка полной валидности дискового образа
    Возвращает (status: bool, message: str)
    """
    # 1. Проверка мета-сектора раздела
    try:
        partition_info = read_partition_record(image_data[:512])
    except ValueError as e:
        return False, f"Ошибка мета-сектора раздела: {str(e)}"
    
    partition_size = len(image_data) // 512

    # Проверка заявленного размера раздела
    if partition_info['size'] + 1 != partition_size:  # +1 мета-сектор
        return False, (f"Размер раздела не соответствует: "
                       f"ожидалось {partition_info['size']} секторов, "
                       f"фактически {partition_size - 1}")

    # 2. Проверка всех мета-секторов
    for sector in range(0, partition_size, 512):
        start = sector * 512
        end = start + 512
        meta = image_data[start:end]

        # Проверка корректности мета-сектора
        if meta[0] == 0x80:  # Файл
            try:
                read_file_record(meta)
            except ValueError as e:
                return False, f"Ошибка файла в секторе {sector}: {str(e)}"
        elif meta[:4] == b'MCFS':  # Дополнительный раздел
            # Если поддерживается вложенность разделов
            pass
        else:
            return False, f"Неверный тип мета-сектора в секторе {sector}"

    # 3. Проверка файлов на пересечения и целостность
    used_sectors = set()
    errors = []

    for sector in range(0, partition_size, 512):
        if sector in used_sectors:
            continue

        meta = image_data[sector*512 : (sector+1)*512]
        if meta[0] != 0x80:
            continue  # Пропуск нефайловых мета-секторов

        try:
            file_info = read_file_record(meta)
            file_end = sector + file_info['size_sectors'] + 1  # +1 мета-сектор

            # Проверка выхода за границы
            if file_end > partition_size:
                errors.append(f"Файл {file_info['name']} выходит за раздел")

            # Проверка пересечения секторов
            for s in range(sector, file_end):
                if s in used_sectors:
                    errors.append(f"Пересечение файлов в секторе {s}")
                used_sectors.add(s)

            # Проверка размера данных
            data_size = file_info['size_sectors'] * 512
            actual_data = image_data[
                (sector + 1)*512 : (sector + 1 + file_info['size_sectors'])*512
            ]
            if len(actual_data) != data_size:
                errors.append(f"Файл {file_info['name']} имеет некорректный размер данных")

        except Exception as e:
            errors.append(f"Ошибка в секторе {sector}: {str(e)}")

    # 4. Проверка свободных секторов
    free_sectors = set(range(partition_size)) - used_sectors
    # TODO: Дополнительные проверки (например, на целостность свободных блоков)

    # 5. Проверка итоговой целостности
    if errors:
        return False, "\n".join(errors)
    else:
        return True, "Изображение диска прошло полную валидацию"

# create [size] [name] [imagename]
def job(args):
    if args[1]=='help ':
        print('create [size] [name]        [image file name]')
        print('add    [file] [in-img name] [image file name]')
        print('list                        [image file name]')
        return 0
    if args[1]=='create':
        size = int(args[2])
        name = args[3]
        vmcfsp = args[4]
        args = 0
        time = 0
        f = create_partition(name,size)
        #print(f'Created partition ({(name, size)})')
        file = open(vmcfsp,'wb')
        file.write(f)
        file.close()
    # python minfs.py add 'exec.px' 'EXECUTABLE' 'image.vmcfsp' ('[0x0001]')
    elif args[1]=='add':
        file_ta = args[2]
        file_tr = args[3]
        vmcfsp = args[4]
        if len(args)>5:
            path = eval(args[5])
        else:
            path = []
        filesrc = open(file_ta,'rb')
        filesrcc = filesrc.read()
        filesrc.close()
        vmcfspf = open(vmcfsp,'rb')
        vmcfspc = bytearray(vmcfspf.read())
        vmcfspf.close()
        # filesrcc  vmcfspc
        file = create_file_record(file_tr,filesrcc,0,1,path = path)
        i = 0
        while vmcfspc[i*512]!=0x00:
            if vmcfspc[i*512]==0x80:
                i+=1+int.from_bytes(byteorder='little',bytes=vmcfspc[i*512+18:i*512+22])
            else:
                i+=1
        place_record(vmcfspc,file,i)
        #print(f'Added file to {vmcfsp}, {(file_ta, len(filesrcc))} (as {file_tr}) -> {(vmcfsp, len(vmcfspc))} (sect {i})')

        vmcfspf = open(vmcfsp,'wb')
        vmcfspf.write(vmcfspc)
        vmcfspf.close()
    # python minfs.py validate 'image.vmcfsp'
    elif args[1]=='validate':
        vmcfsp = args[2]
        vmcfspf = open(vmcfsp,'rb')
        vmcfspc = vmcfspf.read()
        vmcfspf.close()
        status, msg = validate_full_disk_image(vmcfspc)
        if status:
            print("Valid Ok:", msg)
        else:
            print("Valid Error:", msg)
    # python minfs.py list 'image.vmcfsp'
    elif args[1]=='list':
        vmcfsp = args[2]
        vmcfspf = open(vmcfsp,'rb')
        vmcfspc = bytearray(vmcfspf.read())
        vmcfspc = vmcfspc[0x6000:]
        vmcfspf.close()
        i = 0
        print(f'\n  Contents of disk image {vmcfspc[4:20].decode()}\n')
        print('  Type\t  Name\t\t  Attributes\t  Size (B)\t  Size (S)    Path\n')
        while i*512<len(vmcfspc):
            if vmcfspc[i*512]==0x80:
                length = vmcfspc[i*512+18:i*512+22]
                x=read_file_record(vmcfspc[i*512:(i+1)*512])
                print(f"* {x['type']:<8}",end='')
                print(f"{x['name']:<16}",end='')
                print(f"{x['attributes']:<16}",end='')
                print(f"{x['size_sectors']*512:<16}",end='')
                print(f"{x['size_sectors']:<12}",end='')
                print('/',end='')
                for n,j in enumerate(x['path']):
                    if not (all(x == bytearray(b'\x00\x00') for x in x['path'][n:])):
                        print(f"{j.hex()}/",end='')
                print()
                i+=1+int.from_bytes(byteorder='little',bytes=length)
            else:
                i+=1
        print()
args = argv
# python minfs.py create 127 'PhotonOSBR' image.vmcfs}
if len(args)>1:
    job(args)
else:
    print("MinFS (MCFS) Command Interpretator")
    while 1:
        try:
            f = input(">>> ")
            if f=='guide':
                print('create [size] [name]        [image file name]')
                print('add    [file] [in-img name] [image file name]')
                print('list                        [image file name]')
        except:
            break
        try:
            job(['minfs.py']+f.split())
        except IndexError:
            print(f"error: not enough arguments") if f else print(end='')
        except Exception as e:
            print(f"error: {e}")
            print(f"*      {e.args}")

